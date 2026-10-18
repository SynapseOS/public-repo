// ini.c: Чтение INI и поиск параметров
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 09.10.2026
// Последнее изменение: 14.10.2026

#include <synlibc.h>
#include <ini.h>

static char *ini_strdup(const char *s, size_t n) {
	char *p = (char *)malloc(n + 1);

	if (!p) {
		return NULL;
	}

	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static char *trim_inplace(char *s, size_t *out_len) {
	while (*s == ' ' || *s == '\t' || *s == '\r') {
		s++;
	}

	size_t len = strlen(s);

	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) {
		s[--len] = '\0';
	}

	*out_len = len;
	return s;
}

static int strieq(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		char ca = *a, cb = *b;

		if (ca >= 'A' && ca <= 'Z') {
			ca += 32;
		}

		if (cb >= 'A' && cb <= 'Z') {
			cb += 32;
		}

		if (ca != cb) {
			return 0;
		}
	}

	return *a == *b;
}

static ini_section_t *find_or_add_section(ini_file_t *ini, const char *name) {
	for (ini_section_t *s = ini->sections; s; s = s->next) {
		if ((s->name == NULL && name == NULL) || (s->name && name && strcmp(s->name, name) == 0)) {
			return s;
		}
	}

	ini_section_t *ns = (ini_section_t *)malloc(sizeof(ini_section_t));

	if (!ns) {
		return NULL;
	}

	ns->name = name ? ini_strdup(name, strlen(name)) : NULL;
	ns->entries = NULL;
	ns->next = ini->sections;
	ini->sections = ns;
	return ns;
}

static int section_add_kv(ini_section_t *sec, const char *key, size_t klen, const char *val,
                          size_t vlen) {
	for (ini_entry_t *e = sec->entries; e; e = e->next) {
		if (strcmp(e->key, key) == 0) {
			free(e->value);
			e->value = ini_strdup(val, vlen);
			return e->value ? 0 : -1;
		}
	}

	ini_entry_t *ne = (ini_entry_t *)malloc(sizeof(ini_entry_t));

	if (!ne) {
		return -1;
	}

	ne->key = ini_strdup(key, klen);
	ne->value = ini_strdup(val, vlen);

	if (!ne->key || !ne->value) {
		if (ne->key) {
			free(ne->key);
		}

		if (ne->value) {
			free(ne->value);
		}

		free(ne);
		return -1;
	}

	ne->next = sec->entries;
	sec->entries = ne;
	return 0;
}

ini_file_t *ini_parse_buffer(char *buf, size_t size) {
	if (!buf || size == 0) {
		return NULL;
	}

	ini_file_t *ini = (ini_file_t *)malloc(sizeof(ini_file_t));

	if (!ini) {
		return NULL;
	}

	ini->sections = NULL;
	ini->raw = buf;
	ini->raw_size = size;

	ini_section_t *cur = find_or_add_section(ini, NULL); // глобальная

	if (!cur) {
		ini_free(ini);
		return NULL;
	}

	char *p = buf;
	char *end = buf + size;

	while (p < end) {
		// Найти конец строки
		char *line = p;

		while (p < end && *p != '\n') {
			p++;
		}

		if (p < end) {
			*p++ = '\0';
		}

		size_t llen;
		line = trim_inplace(line, &llen);

		if (llen == 0) {
			continue;
		}

		if (line[0] == ';' || line[0] == '#') {
			continue;
		}

		if (line[0] == '[') {
			char *close = strchr(line, ']');

			if (!close) {
				syn_printf("[ini] malformed section: %s\n", line);
				continue;
			}

			*close = '\0';
			char *name = line + 1;
			size_t nlen;
			name = trim_inplace(name, &nlen);
			cur = find_or_add_section(ini, name);

			if (!cur) {
				ini_free(ini);
				return NULL;
			}

			continue;
		}

		// key = value
		char *eq = strchr(line, '=');

		if (!eq) {
			syn_printf("[ini] skipped line: %s\n", line);
			continue;
		}

		*eq = '\0';
		char *key = line;
		char *val = eq + 1;
		size_t klen, vlen;
		key = trim_inplace(key, &klen);
		val = trim_inplace(val, &vlen);

		if (vlen >= 2 && ((val[0] == '"' && val[vlen - 1] == '"') ||
		                  (val[0] == '\'' && val[vlen - 1] == '\''))) {
			val[vlen - 1] = '\0';
			val++;
			vlen -= 2;
		}

		if (klen == 0) {
			continue;
		}

		section_add_kv(cur, key, klen, val, vlen);
	}

	return ini;
}

ini_file_t *ini_parse(const char *path) {
	// O_RDONLY
	int fd = syn_vfs_open(path, 0, 0);

	if (fd < 0) {
		syn_printf("[ini] open failed: %s\n", path);
		return NULL;
	}

	size_t cap = 4096;
	size_t len = 0;
	char *buf = (char *)malloc(cap);

	if (!buf) {
		syn_vfs_close(fd);
		return NULL;
	}

	while (1) {
		ssize_t r = syn_vfs_read(fd, buf + len, cap - len - 1);

		if (r < 0) {
			syn_printf("[ini] read error\n");
			free(buf);
			syn_vfs_close(fd);
			return NULL;
		}

		if (r == 0) {
			break;
		}

		len += (size_t)r;

		if (len + 1 >= cap) {
			size_t ncap = cap * 2;
			char *nb = (char *)malloc(ncap);

			if (!nb) {
				free(buf);
				syn_vfs_close(fd);
				return NULL;
			}

			memcpy(nb, buf, len);
			free(buf);
			buf = nb;
			cap = ncap;
		}
	}

	syn_vfs_close(fd);
	buf[len] = '\0';

	ini_file_t *ini = ini_parse_buffer(buf, len);

	if (!ini) {
		free(buf);
	}

	return ini;
}

static const ini_entry_t *ini_lookup(const ini_file_t *ini, const char *section, const char *key) {
	if (!ini || !key) {
		return NULL;
	}

	for (ini_section_t *s = ini->sections; s; s = s->next) {
		int match = (section == NULL && s->name == NULL) ||
		            (section && s->name && strieq(section, s->name));
		if (!match) {
			continue;
		}

		for (ini_entry_t *e = s->entries; e; e = e->next) {
			if (strieq(e->key, key)) {
				return e;
			}
		}
	}

	return NULL;
}

const char *ini_get(const ini_file_t *ini, const char *section, const char *key) {
	const ini_entry_t *e = ini_lookup(ini, section, key);
	return e ? e->value : NULL;
}

int ini_get_int(const ini_file_t *ini, const char *section, const char *key, int def) {
	const char *v = ini_get(ini, section, key);

	if (!v) {
		return def;
	}

	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
		long r = 0;

		for (const char *p = v + 2; *p; p++) {
			int d;

			if (*p >= '0' && *p <= '9') {
				d = *p - '0';
			} else if (*p >= 'a' && *p <= 'f') {
				d = 10 + (*p - 'a');
			} else if (*p >= 'A' && *p <= 'F') {
				d = 10 + (*p - 'A');
			} else {
				break;
			}

			r = r * 16 + d;
		}

		return (int)r;
	}

	return (int)atol(v);
}

int ini_get_bool(const ini_file_t *ini, const char *section, const char *key, int def) {
	const char *v = ini_get(ini, section, key);

	if (!v) {
		return def;
	}

	if (strieq(v, "1") || strieq(v, "true") || strieq(v, "yes") || strieq(v, "on") ||
	    strieq(v, "enable")) {
		return 1;
	}

	if (strieq(v, "0") || strieq(v, "false") || strieq(v, "no") || strieq(v, "off") ||
	    strieq(v, "disable")) {
		return 0;
	}

	return def;
}

void ini_free(ini_file_t *ini) {
	if (!ini) {
		return;
	}

	ini_section_t *s = ini->sections;

	while (s) {
		ini_section_t *ns = s->next;
		ini_entry_t *e = s->entries;

		while (e) {
			ini_entry_t *ne = e->next;
			free(e->key);
			free(e->value);
			free(e);
			e = ne;
		}

		if (s->name) {
			free(s->name);
		}

		free(s);
		s = ns;
	}

	if (ini->raw) {
		free(ini->raw);
	}

	free(ini);
}

void ini_dump(const ini_file_t *ini) {
	if (!ini) {
		syn_printf("(null ini)\n");
		return;
	}

	for (ini_section_t *s = ini->sections; s; s = s->next) {
		syn_printf("[%s]\n", s->name ? s->name : "");

		for (ini_entry_t *e = s->entries; e; e = e->next) {
			syn_printf("  %s = %s\n", e->key, e->value);
		}
	}
}