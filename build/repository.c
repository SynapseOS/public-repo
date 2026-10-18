// repository.c: проверка архивов и подпись каталога через SSH-agent
// Автор: Елчинян Арен Артурович, 2026, архитектура хоста, ABI POSIX
// Подсистема: выпуск пакетов, первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define PACKAGE_LIMIT (256U * 1024U * 1024U)

// Завершить проверку без выдачи результата
static void fail(const char *message) {
	fprintf(stderr, "repository: %s\n", message);
	exit(EXIT_FAILURE);
}

// Выделить память для ограниченного входного блока
static void *allocate(size_t length) {
	void *result = malloc(length);
	if (result == NULL) {
		fail("out of memory");
	}

	return result;
}

// Прочитать ровно запрошенное число байт
static void read_file(FILE *file, void *data, size_t length) {
	if (fread(data, 1, length, file) != length) {
		fail("truncated file");
	}
}

// Получить размер обычного файла
static size_t file_size(FILE *file, size_t limit) {
	struct stat status;
	if (fstat(fileno(file), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
	    (uintmax_t)status.st_size > limit) {
		fail("invalid file type or size");
	}

	return (size_t)status.st_size;
}

// Открыть входной файл без изменения его содержимого
static FILE *open_input(const char *path) {
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		fail("cannot open input file");
	}

	return file;
}

// Вывести байты в шестнадцатеричном виде
static void print_hex(FILE *file, const unsigned char *bytes, size_t length) {
	for (size_t i = 0; i < length; i++) {
		if (fprintf(file, "%02x", bytes[i]) < 0) {
			fail("cannot write hexadecimal data");
		}
	}
}

// Вывести недоверенный текст как одну строку JSON
static void print_json_string(const unsigned char *bytes, size_t length) {
	putchar('"');
	for (size_t i = 0; i < length; i++) {
		unsigned char byte = bytes[i];
		if (byte < 32 || byte == '"' || byte == '\\') {
			printf("\\u%04x", (unsigned int)byte);
		} else {
			putchar(byte);
		}
	}

	putchar('"');
}

// Посчитать SHA-256 следующего блока файла
static void digest_file(FILE *file, size_t length, unsigned char hash[32]) {
	EVP_MD_CTX *context = EVP_MD_CTX_new( );
	if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256( ), NULL) != 1) {
		fail("cannot initialize SHA-256");
	}

	unsigned char buffer[16384];
	while (length != 0) {
		size_t count = length;
		if (count > sizeof buffer) {
			count = sizeof buffer;
		}

		read_file(file, buffer, count);
		if (EVP_DigestUpdate(context, buffer, count) != 1) {
			fail("cannot update SHA-256");
		}

		length -= count;
	}

	unsigned int count = 0;
	if (EVP_DigestFinal_ex(context, hash, &count) != 1 || count != 32) {
		fail("cannot finish SHA-256");
	}

	EVP_MD_CTX_free(context);
}

// Прочитать восьмеричное поле USTAR без расширений GNU
static uint64_t octal(const unsigned char *field, size_t length) {
	uint64_t result = 0;
	int ended = 0;
	int started = 0;

	for (size_t i = 0; i < length; i++) {
		unsigned char byte = field[i];
		if (byte == 0 || byte == ' ') {
			if (started || byte == 0) {
				ended = 1;
			}
		} else {
			if (ended || byte < '0' || byte > '7' || result > (UINT64_MAX - 7) / 8) {
				fail("invalid USTAR numeric field");
			}

			started = 1;
			result = result * 8 + (uint64_t)(byte - '0');
		}
	}

	return result;
}

// Проверить безопасное имя внутри архива
static void archive_name(const unsigned char *header, char name[257]) {
	size_t prefix = strnlen((const char *)header + 345, 155);
	size_t base = strnlen((const char *)header, 100);
	size_t offset = 0;

	if (prefix != 0) {
		memcpy(name, header + 345, prefix);
		name[prefix] = '/';
		offset = prefix + 1;
	}

	memcpy(name + offset, header, base);
	name[offset + base] = 0;

	for (size_t i = 0; name[i] != 0; i++) {
		unsigned char byte = (unsigned char)name[i];
		if (byte <= 32 || byte >= 127 || byte == '"' || byte == '\\') {
			fail("unsafe USTAR path");
		}
	}
}

// Проверить архив и вывести описание для проверки манифеста через jq
static void inspect(const char *path) {
	FILE *file = open_input(path);
	size_t total = file_size(file, PACKAGE_LIMIT);
	if (total < 1536 || total % 512 != 0) {
		fail("invalid USTAR archive length");
	}

	unsigned char hash[32];
	digest_file(file, total, hash);
	rewind(file);
	printf("{\"size\":%zu,\"sha256\":\"", total);
	print_hex(stdout, hash, sizeof hash);
	printf("\",\"manifest_text\":");

	size_t remaining = total;
	size_t entries = 0;
	size_t zeros = 0;
	while (remaining != 0) {
		unsigned char header[512];
		read_file(file, header, sizeof header);
		remaining -= sizeof header;
		unsigned int sum = 0;
		for (size_t i = 0; i < sizeof header; i++) {
			sum += header[i];
		}

		if (sum == 0) {
			zeros++;
			continue;
		}

		if (zeros != 0 || memcmp(header + 257,
		                         "ustar\0"
		                         "00",
		                         8) != 0) {
			fail("expected plain USTAR archive");
		}

		for (size_t i = 148; i < 156; i++) {
			sum -= header[i];
			sum += ' ';
		}

		if (octal(header + 148, 8) != sum || octal(header + 108, 8) != 0 ||
		    octal(header + 116, 8) != 0 || header[157] != 0) {
			fail("invalid USTAR checksum, ownership or link");
		}

		uint64_t size = octal(header + 124, 12);
		uint64_t mode = octal(header + 100, 8);
		unsigned char type = header[156];
		if (size > remaining || mode > 0777 || (type != 0 && type != '0' && type != '5')) {
			fail("unsupported USTAR entry");
		}

		char name[257];
		archive_name(header, name);
		if (entries == 0) {
			if (strcmp(name, "manifest.json") != 0 || type == '5' || size > 1048576) {
				fail("manifest must be the first regular file");
			}

			char *manifest = allocate((size_t)size + 1);
			read_file(file, manifest, (size_t)size);
			if (memchr(manifest, 0, (size_t)size) != NULL) {
				fail("NUL in manifest");
			}

			manifest[size] = 0;
			print_json_string((const unsigned char *)manifest, (size_t)size);
			printf(",\"entries\":[");
			free(manifest);
		} else {
			if (entries > 512 || strncmp(name, "payload/", 8) != 0 || name[8] == 0) {
				fail("invalid payload entry or file count");
			}

			if (type == '5') {
				if (size != 0) {
					fail("directory has nonzero size");
				}

				size_t length = strlen(name);
				if (name[length - 1] == '/') {
					name[length - 1] = 0;
				}
			}

			digest_file(file, (size_t)size, hash);
			if (entries > 1) {
				putchar(',');
			}

			const char *kind = "file";
			if (type == '5') {
				kind = "directory";
			}

			printf("{\"path\":\"%s\",\"type\":\"%s\",\"mode\":%llu,\"size\":%llu,\"sha256\":\"",
			       name + 8, kind, (unsigned long long)mode, (unsigned long long)size);
			print_hex(stdout, hash, sizeof hash);
			printf("\"}");
		}

		size_t padding = (512 - (size_t)size % 512) % 512;
		if (size + padding > remaining) {
			fail("truncated USTAR padding");
		}

		unsigned char buffer[512];
		read_file(file, buffer, padding);
		for (size_t i = 0; i < padding; i++) {
			if (buffer[i] != 0) {
				fail("nonzero USTAR padding");
			}
		}

		remaining -= (size_t)size + padding;
		entries++;
	}

	if (entries == 0 || zeros < 2 || fgetc(file) != EOF || ferror(file)) {
		fail("invalid USTAR end marker");
	}

	printf("]}\n");
	fclose(file);
}

// Записать целое в сетевом порядке байт
static void put32(unsigned char *data, uint32_t value) {
	data[0] = (unsigned char)(value >> 24);
	data[1] = (unsigned char)(value >> 16);
	data[2] = (unsigned char)(value >> 8);
	data[3] = (unsigned char)value;
}

// Прочитать целое из сетевого порядка байт
static uint32_t get32(const unsigned char *data) {
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) |
	       data[3];
}

// Загрузить открытый Ed25519 ключ из формата OpenSSH
static void public_key(const char *path, unsigned char blob[51]) {
	FILE *file = open_input(path);
	char text[4097];
	size_t length = file_size(file, sizeof text - 1);
	read_file(file, text, length);
	text[length] = 0;
	fclose(file);
	if (memchr(text, 0, length) != NULL) {
		fail("NUL in public key");
	}

	char *state = NULL;
	char *algorithm = strtok_r(text, " \t\r\n", &state);
	char *encoded = strtok_r(NULL, " \t\r\n", &state);
	if (algorithm == NULL || strcmp(algorithm, "ssh-ed25519") != 0 || encoded == NULL ||
	    strlen(encoded) != 68 || EVP_DecodeBlock(blob, (const unsigned char *)encoded, 68) != 51 ||
	    get32(blob) != 11 || memcmp(blob + 4, "ssh-ed25519", 11) != 0 || get32(blob + 15) != 32) {
		fail("expected OpenSSH Ed25519 public key");
	}
}

// Подготовить сообщение формата 2 для подписи
static size_t signing_message(const char *index, unsigned char message[64]) {
	static const char domain[] = "SynapseOS repository index v2";
	FILE *file = open_input(index);
	size_t length = file_size(file, PACKAGE_LIMIT);
	memcpy(message, domain, sizeof domain);
	digest_file(file, length, message + sizeof domain);
	fclose(file);

	return sizeof domain + 32;
}

// Передать или получить весь блок через сокет агента
static void transfer(int socket_fd, unsigned char *data, size_t length, int writing) {
	while (length != 0) {
		ssize_t count;
		if (writing != 0) {
			count = send(socket_fd, data, length, MSG_NOSIGNAL);
		} else {
			count = recv(socket_fd, data, length, 0);
		}

		if (count < 0 && errno == EINTR) {
			continue;
		}

		if (count <= 0) {
			fail("SSH-agent communication failed");
		}

		data += (size_t)count;
		length -= (size_t)count;
	}
}

// Запросить обычную подпись Ed25519 у SSH-agent
static void agent_sign(const unsigned char blob[51], const unsigned char *message, size_t length,
                       unsigned char signature[64]) {
	const char *path = getenv("SSH_AUTH_SOCK");
	struct sockaddr_un address = { 0 };
	if (path == NULL || strlen(path) >= sizeof address.sun_path) {
		fail("SSH_AUTH_SOCK is missing or too long");
	}

	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, strlen(path) + 1);
	int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		fail("cannot create SSH-agent socket");
	}

	struct timeval timeout = { .tv_sec = 60, .tv_usec = 0 };
	if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0 ||
	    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) != 0 ||
	    connect(socket_fd, (const struct sockaddr *)&address, sizeof address) != 0) {
		fail("cannot connect to SSH-agent");
	}

	unsigned char request[132];
	size_t request_length = 1 + 4 + 51 + 4 + length + 4;
	if (request_length + 4 > sizeof request) {
		fail("signing request is too large");
	}

	put32(request, (uint32_t)request_length);
	request[4] = 13;
	put32(request + 5, 51);
	memcpy(request + 9, blob, 51);
	put32(request + 60, (uint32_t)length);
	memcpy(request + 64, message, length);
	put32(request + 64 + length, 0);
	transfer(socket_fd, request, request_length + 4, 1);

	unsigned char response[4096];
	transfer(socket_fd, response, 4, 0);
	uint32_t response_length = get32(response);
	if (response_length != 88) {
		fail("SSH-agent refused the key or returned an invalid signature");
	}

	transfer(socket_fd, response, response_length, 0);
	close(socket_fd);
	if (response[0] != 14 || get32(response + 1) != 83 || get32(response + 5) != 11 ||
	    memcmp(response + 9, "ssh-ed25519", 11) != 0 || get32(response + 20) != 64) {
		fail("invalid SSH-agent Ed25519 response");
	}

	memcpy(signature, response + 24, 64);
}

// Проверить подпись перед записью каталога
static void verify_signature(const unsigned char blob[51], const unsigned char *message,
                             size_t length, const unsigned char signature[64]) {
	EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, blob + 19, 32);
	EVP_MD_CTX *context = EVP_MD_CTX_new( );
	int valid = key != NULL && context != NULL &&
	            EVP_DigestVerifyInit(context, NULL, NULL, NULL, key) == 1 &&
	            EVP_DigestVerify(context, signature, 64, message, length) == 1;

	EVP_MD_CTX_free(context);
	EVP_PKEY_free(key);
	if (valid == 0) {
		fail("invalid Ed25519 signature");
	}
}

// Прочитать шестнадцатеричную запись фиксированного размера
static void read_hex(const char *path, unsigned char *bytes, size_t length) {
	FILE *file = open_input(path);
	for (size_t i = 0; i < length; i++) {
		unsigned int value = 0;
		char pair[3] = { 0 };
		read_file(file, pair, 2);
		if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%2x", &value) != 1) {
			fail("invalid hexadecimal data");
		}

		bytes[i] = (unsigned char)value;
	}

	int byte;
	do {
		byte = fgetc(file);
	} while (byte == '\n' || byte == '\r');

	if (byte != EOF || ferror(file)) {
		fail("trailing hexadecimal data");
	}

	fclose(file);
}

// Выбрать проверку архива, открытого ключа или подпись индекса
int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
		inspect(argv[2]);
	} else if (argc == 3 && strcmp(argv[1], "key") == 0) {
		unsigned char blob[51];
		public_key(argv[2], blob);
		print_hex(stdout, blob + 19, 32);
		putchar('\n');
	} else if ((argc == 6 && strcmp(argv[1], "sign") == 0) ||
	           (argc == 5 && strcmp(argv[1], "verify") == 0)) {
		unsigned char blob[51], message[64], signature[64];
		public_key(argv[2], blob);
		int signing = strcmp(argv[1], "sign") == 0;
		const char *index = argv[3];
		const char *signature_path = argv[4];
		if (signing != 0) {
			if (strcmp(argv[3], "-") != 0) {
				unsigned char trusted[32];
				read_hex(argv[3], trusted, sizeof trusted);
				if (memcmp(trusted, blob + 19, sizeof trusted) != 0) {
					fail("selected key does not match the trusted repository key");
				}
			}

			index = argv[4];
			signature_path = argv[5];
		}

		size_t length = signing_message(index, message);
		if (signing != 0) {
			agent_sign(blob, message, length, signature);
		} else {
			read_hex(signature_path, signature, sizeof signature);
		}

		verify_signature(blob, message, length, signature);
		if (signing != 0) {
			FILE *output = fopen(signature_path, "wx");
			if (output == NULL) {
				fail("cannot create signature file");
			}

			print_hex(output, signature, sizeof signature);
			if (fclose(output) != 0) {
				fail("cannot finish signature file");
			}
		}
	} else {
		fail("usage: repository inspect ARCHIVE | key PUB | "
		     "sign PUB TRUSTED INDEX SIG | verify PUB INDEX SIG");
	}

	if (fflush(stdout) != 0 || ferror(stdout)) {
		fail("cannot write result");
	}

	return EXIT_SUCCESS;
}
