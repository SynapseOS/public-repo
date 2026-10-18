// synapse-web.inc: асинхронные запросы и хранилище сайтов для Duktape
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl, NetSurf и libcurl
// Подсистема: веб-интерфейсы браузера
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <curl/curl.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "content/urldb.h"
#include "content/content_protected.h"

#define SYN_WEB_ROOT                                                                               \
	"\xff"                                                                                         \
	"SynapseRequests"
#define SYN_WEB_OWNER                                                                              \
	"\xff"                                                                                         \
	"SynapseOwner"
#define SYN_WEB_LIMIT (8U * 1024U * 1024U)
#define SYN_STORAGE_LIMIT (1024U * 1024U)

struct syn_web_request {
	struct syn_web_request *next;
	struct synapse_web *web;
	CURL *curl;
	struct curl_slist *headers;
	nsurl *url;
	char *body;
	size_t body_length;
	char *response;
	size_t response_length;
	char response_headers[65536];
	size_t header_length;
	char allow_origin[2048];
	char method[16];
	unsigned id;
	bool cross_origin;
	bool cookies;
};

struct synapse_web {
	window_private_t *owner;
	duk_context *ctx;
	CURLM *multi;
	struct syn_web_request *requests;
	unsigned next_id;
	unsigned count;
	bool running;
	bool closing;
};

static void syn_web_poll(void *data);

// Получить живое окно через удерживаемый функцией объект JavaScript
static window_private_t *syn_web_owner(duk_context *ctx) {
	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, SYN_WEB_OWNER);
	duk_get_prop_string(ctx, -1, dukky_magic_string_private);
	window_private_t *owner = duk_get_pointer(ctx, -1);
	duk_pop_3(ctx);

	if (owner == NULL || owner->closed_down) {
		(void)duk_error(ctx, DUK_ERR_ERROR, "InvalidStateError: window is closed");
		return NULL;
	}

	return owner;
}

// Разрешить только сетевые URL без встроенного пароля
static bool syn_web_url_allowed(nsurl *url) {
	lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
	bool allowed = scheme != NULL && (strcmp(lwc_string_data(scheme), "http") == 0 ||
	                                  strcmp(lwc_string_data(scheme), "https") == 0);
	if (scheme != NULL) {
		lwc_string_unref(scheme);
	}

	return allowed && nsurl_has_component(url, NSURL_HOST) &&
	       !nsurl_has_component(url, NSURL_CREDENTIALS);
}

// Сформировать имя сайта без пути, данных входа и параметров запроса
static char *syn_web_origin(nsurl *url) {
	char *origin = NULL;
	size_t length = 0;

	if (!syn_web_url_allowed(url) ||
	    nsurl_get(url, NSURL_SCHEME | NSURL_HOST | NSURL_PORT, &origin, &length) != NSERROR_OK) {
		return NULL;
	}

	return origin;
}

// Освободить запрос только после удаления из curl multi
static void syn_web_request_free(struct syn_web_request *request) {
	curl_easy_cleanup(request->curl);
	curl_slist_free_all(request->headers);
	nsurl_unref(request->url);
	free(request->body);
	free(request->response);
	free(request);
}

// Освободить отменённые операции, не вызывая закрытые контексты JavaScript
static void syn_web_destroy(struct synapse_web *web) {
	while (web->requests != NULL) {
		struct syn_web_request *request = web->requests;
		web->requests = request->next;
		curl_multi_remove_handle(web->multi, request->curl);
		syn_web_request_free(request);
	}

	curl_multi_cleanup(web->multi);
	free(web);
}

// Отменить запросы при переходе со страницы или закрытии окна
static void syn_web_close(duk_context *ctx, window_private_t *owner) {
	struct synapse_web *web = owner->web;

	if (web == NULL) {
		return;
	}

	owner->web = NULL;
	web->closing = true;
	guit->misc->schedule(-1, syn_web_poll, web);
	duk_push_object(ctx);
	duk_put_global_string(ctx, SYN_WEB_ROOT);

	if (!web->running) {
		syn_web_destroy(web);
	}
}

// Ограничить память ответа, не допуская переполнения размера
static size_t syn_web_data(char *bytes, size_t size, size_t count, void *data) {
	struct syn_web_request *request = data;

	if (size != 0 && count > SYN_WEB_LIMIT / size) {
		return 0;
	}

	size_t length = size * count;

	if (length > SYN_WEB_LIMIT - request->response_length) {
		return 0;
	}

	char *buffer = realloc(request->response, request->response_length + length + 1);

	if (buffer == NULL) {
		return 0;
	}

	request->response = buffer;
	memcpy(buffer + request->response_length, bytes, length);
	request->response_length += length;
	buffer[request->response_length] = 0;
	return length;
}

// Сохранить заголовки последнего ответа, исключив закрытые данные cookies
static size_t syn_web_header(char *bytes, size_t size, size_t count, void *data) {
	struct syn_web_request *request = data;

	if (size != 0 && count > sizeof(request->response_headers) / size) {
		return 0;
	}

	size_t length = size * count;

	if (length >= sizeof(request->response_headers) - request->header_length) {
		return 0;
	}

	if (length >= 5 && strncmp(bytes, "HTTP/", 5) == 0) {
		request->header_length = 0;
		request->allow_origin[0] = 0;
	}

	if (length > 11 && strncasecmp(bytes, "Set-Cookie:", 11) == 0) {
		if (request->cookies) {
			char *cookie = strndup(bytes + 11, length - 11);

			if (cookie == NULL) {
				return 0;
			}

			urldb_set_cookie(cookie, request->url, NULL);
			free(cookie);
		}

		return length;
	}

	if (length > 28 && strncasecmp(bytes, "Access-Control-Allow-Origin:", 28) == 0) {
		size_t start = 28;
		size_t end = length;

		while (start < end && (bytes[start] == ' ' || bytes[start] == '\t')) {
			start++;
		}
		while (end > start &&
		       (bytes[end - 1] == '\r' || bytes[end - 1] == '\n' || bytes[end - 1] == ' ')) {
			end--;
		}

		if (end - start >= sizeof(request->allow_origin) || request->allow_origin[0] != 0) {
			return 0;
		}

		memcpy(request->allow_origin, bytes + start, end - start);
		request->allow_origin[end - start] = 0;
	}

	if (request->cross_origin && !(length > 13 && strncasecmp(bytes, "Content-Type:", 13) == 0) &&
	    !(length > 15 && strncasecmp(bytes, "Content-Length:", 15) == 0) &&
	    !(length > 14 && strncasecmp(bytes, "Cache-Control:", 14) == 0) &&
	    !(length > 17 && strncasecmp(bytes, "Content-Language:", 17) == 0) &&
	    !(length > 8 && strncasecmp(bytes, "Expires:", 8) == 0) &&
	    !(length > 14 && strncasecmp(bytes, "Last-Modified:", 14) == 0) &&
	    !(length > 7 && strncasecmp(bytes, "Pragma:", 7) == 0)) {
		return length;
	}

	memcpy(request->response_headers + request->header_length, bytes, length);
	request->header_length += length;
	request->response_headers[request->header_length] = 0;
	return length;
}

// Передать завершение запроса и удалить удерживаемый обработчик
static void syn_web_deliver(struct syn_web_request *request, CURLcode result) {
	struct synapse_web *web = request->web;
	duk_context *ctx = web->ctx;
	duk_idx_t top = duk_get_top(ctx);
	long status = 0;
	curl_easy_getinfo(request->curl, CURLINFO_RESPONSE_CODE, &status);
	const char *error = result == CURLE_OK ? "" : curl_easy_strerror(result);
	char *origin = syn_web_origin(content_get_url(&web->owner->htmlc->base));

	if (request->cross_origin && (origin == NULL || (strcmp(request->allow_origin, "*") != 0 &&
	                                                 strcmp(request->allow_origin, origin) != 0))) {
		error = "CORS response blocked";
	}

	free(origin);
	duk_get_global_string(ctx, SYN_WEB_ROOT);
	duk_get_prop_index(ctx, -1, request->id);
	duk_del_prop_index(ctx, -2, request->id);
	duk_remove(ctx, -2);

	if (duk_is_callable(ctx, -1)) {
		duk_push_string(ctx, error);
		bool success = error[0] == 0;
		duk_push_int(ctx, success ? (int)status : 0);
		duk_push_lstring(ctx, success ? request->response_headers : "",
		                 success ? request->header_length : 0);
		duk_push_lstring(ctx, success && request->response != NULL ? request->response : "",
		                 success ? request->response_length : 0);
		duk_push_string(ctx, nsurl_access(request->url));
		syn_web_request_free(request);
		dukky_pcall(ctx, 5, true);
	} else {
		syn_web_request_free(request);
	}

	duk_set_top(ctx, top);
}

// Обслужить сетевые операции без ожидания ответа в потоке интерфейса
static void syn_web_poll(void *data) {
	struct synapse_web *web = data;
	int active = 0;
	web->running = true;
	CURLMcode result = curl_multi_perform(web->multi, &active);

	if (result != CURLM_OK) {
		web->closing = true;
		web->owner->web = NULL;
	}

	int queued = 0;
	CURLMsg *message;

	while (!web->closing && (message = curl_multi_info_read(web->multi, &queued)) != NULL) {
		if (message->msg != CURLMSG_DONE) {
			continue;
		}

		struct syn_web_request *request = NULL;
		curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);

		if (request == NULL) {
			continue;
		}

		struct syn_web_request **link = &web->requests;

		while (*link != NULL && *link != request) {
			link = &(*link)->next;
		}

		if (*link == NULL) {
			continue;
		}

		*link = request->next;
		web->count--;
		CURLcode status = message->data.result;
		curl_multi_remove_handle(web->multi, request->curl);
		syn_web_deliver(request, status);
	}

	web->running = false;

	if (web->closing) {
		syn_web_destroy(web);
	} else if (web->requests != NULL && guit->misc->schedule(10, syn_web_poll, web) != NSERROR_OK) {
		syn_web_close(web->ctx, web->owner);
	}
}

// Проверить имя заголовка и запретить подмену служебных заголовков браузера
static bool syn_web_header_allowed(const char *name) {
	static const char *blocked[] = {
		"host",    "cookie",          "cookie2",        "origin",
		"referer", "connection",      "content-length", "transfer-encoding",
		"upgrade", "accept-encoding", "expect",         "trailer",
		"te"
	};

	if (name[0] == 0 || strlen(name) > 128 || strncasecmp(name, "sec-", 4) == 0 ||
	    strncasecmp(name, "proxy-", 6) == 0 || strncasecmp(name, "access-control-", 15) == 0) {
		return false;
	}

	for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
		if (strcasecmp(name, blocked[i]) == 0) {
			return false;
		}
	}

	for (const unsigned char *p = (const unsigned char *)name; *p != 0; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		      strchr("!#$%&'*+-.^_`|~", *p) != NULL)) {
			return false;
		}
	}

	return true;
}

// Запустить настоящий HTTP-запрос, возвращая номер для отмены
static duk_ret_t syn_web_start(duk_context *ctx) {
	window_private_t *owner = syn_web_owner(ctx);

	if (owner == NULL) {
		return DUK_RET_ERROR;
	}

	const char *address = duk_require_string(ctx, 0);
	const char *method = duk_require_string(ctx, 1);
	duk_size_t body_length = 0;
	const char *body = duk_require_lstring(ctx, 2, &body_length);
	duk_require_callable(ctx, 4);
	duk_size_t header_size = 0;
	const char *header_text = duk_require_lstring(ctx, 3, &header_size);

	if (header_size > 65536 || strlen(header_text) != header_size || strlen(address) > 8192 ||
	    strlen(method) >= 16 || body_length > 2097152) {
		return duk_error(ctx, DUK_ERR_TYPE_ERROR, "Unsupported request");
	}

	static const char *methods[] = { "GET", "HEAD", "POST", "PUT", "DELETE", "PATCH", "OPTIONS" };
	bool allowed = false;

	for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		allowed |= strcmp(method, methods[i]) == 0;
	}

	if (!allowed ||
	    ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) && body_length != 0)) {
		return duk_error(ctx, DUK_ERR_TYPE_ERROR, "Invalid HTTP method or body");
	}

	nsurl *url = NULL;
	nsurl *origin_url = content_get_url(&owner->htmlc->base);

	if (nsurl_join(owner->htmlc->base_url, address, &url) != NSERROR_OK ||
	    !syn_web_url_allowed(url)) {
		if (url != NULL) {
			nsurl_unref(url);
		}

		return duk_error(ctx, DUK_ERR_TYPE_ERROR, "Invalid HTTP URL");
	}

	char *origin = syn_web_origin(origin_url);
	bool cross = !nsurl_compare(origin_url, url, NSURL_SCHEME | NSURL_HOST | NSURL_PORT);
	int credentials = duk_get_int(ctx, 5);
	const char *mode = duk_get_string(ctx, 7);
	bool cookies = credentials != 0 && !cross;
	// Сложные запросы к чужим сайтам требуют ещё не реализованного preflight
	if (origin == NULL || (cross && credentials == 2) ||
	    (cross && mode != NULL && strcmp(mode, "same-origin") == 0) ||
	    (cross && strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) ||
	    (strncmp(nsurl_access(origin_url), "https:", 6) == 0 &&
	     strncmp(nsurl_access(url), "http:", 5) == 0)) {
		nsurl_unref(url);
		free(origin);
		return duk_error(ctx, DUK_ERR_TYPE_ERROR, "Origin or mixed content policy blocked request");
	}

	struct curl_slist *headers = NULL;
	bool valid = true;
	const char *cursor = header_text;
	unsigned header_count = 0;

	while (valid && *cursor != 0) {
		const char *end = strstr(cursor, "\r\n");

		if (end == NULL) {
			end = cursor + strlen(cursor);
		}

		const char *colon = memchr(cursor, ':', (size_t)(end - cursor));
		valid = colon != NULL && colon > cursor && colon - cursor <= 128 && end - colon < 4096 &&
		        ++header_count <= 64 && !cross;
		if (!valid) {
			break;
		}

		char name[129];
		memcpy(name, cursor, (size_t)(colon - cursor));
		name[colon - cursor] = 0;
		valid = syn_web_header_allowed(name);

		for (const char *byte = colon + 1; valid && byte < end; byte++) {
			valid = *byte != '\r' && *byte != '\n';
		}

		if (valid) {
			char line[4352];
			size_t bytes = (size_t)(end - cursor);
			memcpy(line, cursor, bytes);
			line[bytes] = 0;
			struct curl_slist *updated = curl_slist_append(headers, line);
			valid = updated != NULL;

			if (valid) {
				headers = updated;
			}
		}

		cursor = *end == 0 ? end : end + 2;
	}

	if (valid && cross) {
		char line[2304];
		snprintf(line, sizeof(line), "Origin: %s", origin);
		struct curl_slist *updated = curl_slist_append(headers, line);
		valid = updated != NULL;

		if (valid) {
			headers = updated;
		}
	}

	free(origin);

	if (!valid) {
		nsurl_unref(url);
		curl_slist_free_all(headers);
		return duk_error(ctx, DUK_ERR_TYPE_ERROR, "Invalid or unsupported request headers");
	}

	if (owner->web == NULL) {
		struct synapse_web *web = calloc(1, sizeof(*web));

		if (web != NULL) {
			web->multi = curl_multi_init( );
		}

		if (web == NULL || web->multi == NULL) {
			free(web);
			nsurl_unref(url);
			curl_slist_free_all(headers);
			return duk_error(ctx, DUK_ERR_ERROR, "Network allocation failed");
		}

		web->owner = owner;
		web->ctx = ctx;
		owner->web = web;
	}

	struct synapse_web *web = owner->web;
	struct syn_web_request *request = web->count < 16 ? calloc(1, sizeof(*request)) : NULL;

	if (request == NULL) {
		nsurl_unref(url);
		curl_slist_free_all(headers);
		return duk_error(ctx, DUK_ERR_ERROR, "Too many requests or allocation failed");
	}

	request->web = web;
	request->url = url;
	request->headers = headers;
	request->cross_origin = cross;
	request->cookies = cookies;
	request->id = ++web->next_id;
	request->body = malloc(body_length + 1);
	request->curl = curl_easy_init( );

	if (request->body == NULL || request->curl == NULL) {
		syn_web_request_free(request);
		return duk_error(ctx, DUK_ERR_ERROR, "Network allocation failed");
	}

	memcpy(request->body, body, body_length);
	request->body[body_length] = 0;
	request->body_length = body_length;
	strcpy(request->method, method);
	CURL *curl = request->curl;
	CURLcode code = CURLE_OK;
#define SYN_CURL(option, value)                                                                    \
	do {                                                                                           \
		if (code == CURLE_OK) {                                                                    \
			code = curl_easy_setopt(curl, option, value);                                          \
		}                                                                                          \
	} while (0)
	SYN_CURL(CURLOPT_URL, nsurl_access(url));
	SYN_CURL(CURLOPT_PROTOCOLS_STR, "http,https");
	SYN_CURL(CURLOPT_FOLLOWLOCATION, 0L);
	SYN_CURL(CURLOPT_TIMEOUT_MS, (long)(duk_get_int(ctx, 6) > 0 ? duk_get_int(ctx, 6) : 60000));
	SYN_CURL(CURLOPT_CONNECTTIMEOUT_MS, 15000L);
	SYN_CURL(CURLOPT_NOSIGNAL, 1L);
	SYN_CURL(CURLOPT_SSL_VERIFYPEER, 1L);
	SYN_CURL(CURLOPT_SSL_VERIFYHOST, 2L);
	SYN_CURL(CURLOPT_ACCEPT_ENCODING, "");
	SYN_CURL(CURLOPT_USERAGENT, "NetSurf/3.11 SynapseOS");
	SYN_CURL(CURLOPT_HTTPHEADER, request->headers);
	SYN_CURL(CURLOPT_WRITEFUNCTION, syn_web_data);
	SYN_CURL(CURLOPT_WRITEDATA, request);
	SYN_CURL(CURLOPT_HEADERFUNCTION, syn_web_header);
	SYN_CURL(CURLOPT_HEADERDATA, request);
	SYN_CURL(CURLOPT_PRIVATE, request);

	if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
		SYN_CURL(CURLOPT_POSTFIELDS, request->body);
		SYN_CURL(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_length);
	}

	SYN_CURL(CURLOPT_CUSTOMREQUEST, method);
	SYN_CURL(CURLOPT_NOBODY, strcmp(method, "HEAD") == 0 ? 1L : 0L);

	if (cookies) {
		char *cookie = urldb_get_cookie(url, true);

		if (cookie != NULL) {
			SYN_CURL(CURLOPT_COOKIE, cookie);
			free(cookie);
		}
	}
#undef SYN_CURL
	if (code != CURLE_OK || curl_multi_add_handle(web->multi, curl) != CURLM_OK) {
		syn_web_request_free(request);
		return duk_error(ctx, DUK_ERR_ERROR, "Network setup failed");
	}

	request->next = web->requests;
	web->requests = request;
	web->count++;
	duk_get_global_string(ctx, SYN_WEB_ROOT);
	duk_dup(ctx, 4);
	duk_put_prop_index(ctx, -2, request->id);
	duk_pop(ctx);

	if (guit->misc->schedule(0, syn_web_poll, web) != NSERROR_OK) {
		syn_web_close(ctx, owner);
		return duk_error(ctx, DUK_ERR_ERROR, "Network scheduling failed");
	}

	duk_push_uint(ctx, request->id);
	return 1;
}

// Отменить передачу и освободить её обработчик без вызова JavaScript
static duk_ret_t syn_web_abort(duk_context *ctx) {
	window_private_t *owner = syn_web_owner(ctx);

	if (owner == NULL) {
		return DUK_RET_ERROR;
	}

	unsigned id = duk_require_uint(ctx, 0);
	struct synapse_web *web = owner->web;

	if (web != NULL) {
		struct syn_web_request **link = &web->requests;

		while (*link != NULL) {
			struct syn_web_request *request = *link;

			if (request->id == id) {
				*link = request->next;
				web->count--;
				curl_multi_remove_handle(web->multi, request->curl);
				syn_web_request_free(request);
				duk_get_global_string(ctx, SYN_WEB_ROOT);
				duk_del_prop_index(ctx, -1, id);
				duk_pop(ctx);
				break;
			}

			link = &request->next;
		}
	}

	return 0;
}

// Создать закрытый каталог и имя файла, зависящее только от сайта
static bool syn_storage_path(window_private_t *owner, char *path, size_t capacity) {
	char *origin = syn_web_origin(content_get_url(&owner->htmlc->base));
	const char *home = getenv("HOME");

	if (origin == NULL || home == NULL) {
		free(origin);
		return false;
	}

	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256((const unsigned char *)origin, strlen(origin), digest);
	free(origin);
	char folder[1024];
	int count = snprintf(folder, sizeof(folder), "%s/.netsurf", home);

	if (count < 0 || (size_t)count >= sizeof(folder)) {
		return false;
	}

	if (mkdir(folder, 0700) != 0 && errno != EEXIST) {
		return false;
	}

	count = snprintf(folder, sizeof(folder), "%s/.netsurf/local-storage", home);

	if (count < 0 || (size_t)count >= sizeof(folder) ||
	    (mkdir(folder, 0700) != 0 && errno != EEXIST)) {
		return false;
	}

	struct stat status;

	if (lstat(folder, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != getuid( ) ||
	    (status.st_mode & 077) != 0) {
		return false;
	}

	char hex[SHA256_DIGEST_LENGTH * 2 + 1];

	for (size_t i = 0; i < sizeof(digest); i++) {
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	}

	count = snprintf(path, capacity, "%s/%s.json", folder, hex);
	return count >= 0 && (size_t)count < capacity;
}

// Прочитать ограниченный файл localStorage текущего сайта
static duk_ret_t syn_storage_read(duk_context *ctx) {
	window_private_t *owner = syn_web_owner(ctx);

	if (owner == NULL) {
		return DUK_RET_ERROR;
	}

	char path[1200];

	if (!syn_storage_path(owner, path, sizeof(path))) {
		return duk_error(ctx, DUK_ERR_ERROR, "SecurityError: storage unavailable");
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0) {
		if (errno == ENOENT) {
			duk_push_string(ctx, "{}");
			return 1;
		}

		return duk_error(ctx, DUK_ERR_ERROR, "Storage read failed");
	}

	struct stat status;

	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
	    status.st_size > SYN_STORAGE_LIMIT) {
		close(fd);
		return duk_error(ctx, DUK_ERR_ERROR, "Storage file invalid");
	}

	size_t length = (size_t)status.st_size;
	char *bytes = malloc(length + 1);

	if (bytes == NULL) {
		close(fd);
		return duk_error(ctx, DUK_ERR_ERROR, "Storage allocation failed");
	}

	size_t offset = 0;

	while (offset < length) {
		ssize_t received = read(fd, bytes + offset, length - offset);

		if (received < 0 && errno == EINTR) {
			continue;
		}

		if (received <= 0) {
			free(bytes);
			close(fd);
			return duk_error(ctx, DUK_ERR_ERROR, "Storage read incomplete");
		}

		offset += (size_t)received;
	}

	close(fd);
	duk_push_lstring(ctx, bytes, length);
	free(bytes);
	return 1;
}

// Записать localStorage заменой готового файла, сохранив прежние данные при ошибке
static duk_ret_t syn_storage_write(duk_context *ctx) {
	window_private_t *owner = syn_web_owner(ctx);

	if (owner == NULL) {
		return DUK_RET_ERROR;
	}

	duk_size_t length = 0;
	const char *bytes = duk_require_lstring(ctx, 0, &length);
	char path[1200];
	char temporary[1250];

	if (length > SYN_STORAGE_LIMIT) {
		return duk_error(ctx, DUK_ERR_ERROR, "QuotaExceededError: storage limit is 1 MiB");
	}

	if (!syn_storage_path(owner, path, sizeof(path))) {
		return duk_error(ctx, DUK_ERR_ERROR, "SecurityError: storage unavailable");
	}

	snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
	int fd = mkstemp(temporary);

	if (fd < 0) {
		return duk_error(ctx, DUK_ERR_ERROR, "Storage file creation failed");
	}

	bool success = fchmod(fd, 0600) == 0;
	size_t offset = 0;

	while (success && offset < length) {
		ssize_t written = write(fd, bytes + offset, length - offset);

		if (written < 0 && errno == EINTR) {
			continue;
		}

		if (written <= 0) {
			success = false;
		} else {
			offset += (size_t)written;
		}
	}

	if (success) {
		success = fsync(fd) == 0;
	}

	if (close(fd) != 0) {
		success = false;
	}

	if (success) {
		success = rename(temporary, path) == 0;
	}

	if (!success) {
		unlink(temporary);
		return duk_error(ctx, DUK_ERR_ERROR, "Storage write failed");
	}

	return 0;
}

// Привязать функции к объекту окна, чтобы его память жила до последнего вызова
static void syn_web_install(duk_context *ctx) {
	static const struct {
		const char *name;
		duk_c_function function;
		int args;
	} functions[] = { { "__synapseHTTP", syn_web_start, 8 },
		              { "__synapseAbort", syn_web_abort, 1 },
		              { "__synapseStorageRead", syn_storage_read, 0 },
		              { "__synapseStorageWrite", syn_storage_write, 1 } };

	duk_push_object(ctx);
	duk_put_prop_string(ctx, 0, SYN_WEB_ROOT);

	for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
		duk_push_c_function(ctx, functions[i].function, functions[i].args);
		duk_dup(ctx, 0);
		duk_put_prop_string(ctx, -2, SYN_WEB_OWNER);
		duk_put_prop_string(ctx, 0, functions[i].name);
	}
}
