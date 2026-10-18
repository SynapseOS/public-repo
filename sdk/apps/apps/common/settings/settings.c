// settings.c: Чтение, проверка и сохранение системного INI
// Автор: Елчинян Арен Артурович, 2026
// Проверяем известные ключи и сохраняем остальные строки INI без изменений
// Подсистема: общая конфигурация пользовательских программ
// Создан: 12.10.2026
// Последнее изменение: 14.10.2026

#define _GNU_SOURCE
#include "settings.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define SYS "/etc/settings.ini"
#define FIELD(file, sec, key, en, ru, def, choices, type, min, max, effect)                        \
	{ file, sec, key, en, ru, def, choices, effect, type, min, max }
const struct setting_spec setting_specs[] = {
	FIELD(SYS, "System", "Language", "Language", "Язык", "ru", "ru|en", SET_CHOICE, 0, 0,
	      "session"),
	FIELD(SYS, "System", "Hostname", "Hostname", "Имя компьютера", "synapseos", NULL, SET_TEXT, 0,
	      0, "reboot"),
	FIELD(SYS, "System", "Timezone", "Timezone", "Часовой пояс", "Europe/Moscow", NULL, SET_TEXT, 0,
	      0, "reboot"),
	FIELD(SYS, "Boot", "DefaultShell", "Default shell", "Оболочка консоли", "/bin/sh", NULL,
	      SET_PATH, 0, 0, "session"),
	FIELD(SYS, "Network", "Mode", "Network mode", "Режим сети", "DHCP", "DHCP|Static", SET_CHOICE,
	      0, 0, "reboot"),
	FIELD(SYS, "Network", "IP", "IP address", "Адрес IP", "", NULL, SET_TEXT, 0, 0, "reboot"),
	FIELD(SYS, "Network", "Subnet", "Subnet", "Маска сети", "", NULL, SET_TEXT, 0, 0, "reboot"),
	FIELD(SYS, "Network", "Gateway", "Gateway", "Шлюз", "", NULL, SET_TEXT, 0, 0, "reboot"),
	FIELD(SYS, "DNS", "1", "DNS server 1", "Сервер DNS 1", "77.88.8.8", NULL, SET_TEXT, 0, 0,
	      "reboot"),
	FIELD(SYS, "DNS", "2", "DNS server 2", "Сервер DNS 2", "62.76.76.62", NULL, SET_TEXT, 0, 0,
	      "reboot"),
	FIELD(SYS, "SSH", "Enabled", "SSH enabled", "Включить SSH", "0", NULL, SET_BOOL, 0, 1,
	      "reboot"),
	FIELD(SYS, "SSH", "Port", "SSH port", "Порт SSH", "22", NULL, SET_INT, 1, 65535, "reboot"),
	FIELD(SYS, "GuestAgent", "Enabled", "Guest agent", "Агент виртуальной машины", "1", NULL,
	      SET_BOOL, 0, 1, "reboot"),
	FIELD(SYS, "FTP", "Enabled", "FTP enabled", "Включить FTP", "0", NULL, SET_BOOL, 0, 1,
	      "reboot"),
	FIELD(SYS, "Desktop", "Enabled", "Graphical session", "Графическая сессия", "0", NULL, SET_BOOL,
	      0, 1, "reboot"),
	FIELD(SYS, "Desktop", "Path", "Session executable", "Программа сессии", "/bin/sh", NULL,
	      SET_PATH, 0, 0, "reboot"),
	FIELD(SYS, "Terminal", "Font", "Console font", "Шрифт консоли", "/assets/fonts/dejavu-mono.psf",
	      NULL, SET_PATH, 0, 0, "session"),

};
const int setting_count = sizeof(setting_specs) / sizeof(setting_specs[0]);
static const char *files[] = { SYS };

static int file_id(const char *path) {
	return strcmp(path, SYS) != 0;
}

static int make_path(struct settings *s, const char *file, char *out, size_t cap) {
	if (snprintf(out, cap, "%s%s", s->root, file) >= (int)cap) {
		errno = ENAMETOOLONG;
		return -1;
	}

	return 0;
}

static char *trim(char *p) {
	while (isspace((unsigned char)*p)) {
		++p;
	}

	char *end = p + strlen(p);

	while (end > p && isspace((unsigned char)end[-1])) {
		*--end = 0;
	}

	return p;
}

static unsigned long hash(const char *p, size_t n) {
	unsigned long h = 1469598103934665603UL;

	for (size_t i = 0; i < n; ++i) {
		h = (h ^ (unsigned char)p[i]) * 1099511628211UL;
	}

	return h;
}

static char *read_file(const char *path, bool *exists, unsigned long *h) {
	*exists = false;
	*h = 0;
	FILE *f = fopen(path, "rb");

	if (!f) {
		if (errno == ENOENT) {
			return strdup("");
		}

		return NULL;
	}

	struct stat st;

	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size > 1024 * 1024) {
		fclose(f);
		errno = EINVAL;
		return NULL;
	}

	size_t n = (size_t)st.st_size;
	char *text = malloc(n + 1);

	if (!text) {
		fclose(f);
		return NULL;
	}

	if (fread(text, 1, n, f) != n || memchr(text, 0, n)) {
		free(text);
		fclose(f);
		errno = EIO;
		return NULL;
	}

	text[n] = 0;
	fclose(f);
	*exists = true;
	*h = hash(text, n);
	return text;
}

int settings_find(const char *section, const char *key) {
	for (int i = 0; i < setting_count; ++i) {
		if (!strcmp(setting_specs[i].section, section) && !strcmp(setting_specs[i].key, key)) {
			return i;
		}
	}

	return -1;
}

static int find_in(const char *file, const char *section, const char *key) {
	for (int i = 0; i < setting_count; ++i) {
		if (!strcmp(setting_specs[i].file, file) && !strcmp(setting_specs[i].section, section) &&
		    !strcmp(setting_specs[i].key, key)) {
			return i;
		}
	}

	return -1;
}

int settings_set(struct settings *s, int i, const char *v) {
	if (i < 0 || i >= setting_count || !v || strlen(v) >= SETTINGS_VALUE || strpbrk(v, "\r\n")) {
		goto invalid;
	}

	const struct setting_spec *spec = &setting_specs[i];

	if (spec->type == SET_BOOL && strcmp(v, "0") && strcmp(v, "1")) {
		goto invalid;
	}

	if (spec->type == SET_INT) {
		char *end;
		errno = 0;
		long n = strtol(v, &end, 10);

		if (errno || !*v || *end || n < spec->min || n > spec->max) {
			goto invalid;
		}
	}

	if (*v && (!strcmp(spec->section, "DNS") ||
	           (!strcmp(spec->section, "Network") &&
	            (!strcmp(spec->key, "IP") || !strcmp(spec->key, "Subnet") ||
	             !strcmp(spec->key, "Gateway"))))) {
		struct in_addr address;

		if (inet_pton(AF_INET, v, &address) != 1) {
			goto invalid;
		}
	}

	if (spec->type == SET_PATH && *v != '/') {
		goto invalid;
	}

	if (spec->type == SET_COLOR) {
		if (strlen(v) != 7 || *v != '#') {
			goto invalid;
		}

		for (int j = 1; j < 7; ++j) {
			if (!isxdigit((unsigned char)v[j])) {
				goto invalid;
			}
		}
	}

	if (spec->type == SET_CHOICE) {
		char choices[128];
		snprintf(choices, sizeof(choices), "%s", spec->choices);
		bool found = false;
		char *save, *p = strtok_r(choices, "|", &save);

		for (; p; p = strtok_r(NULL, "|", &save)) {
			if (!strcmp(v, p)) {
				found = true;
			}
		}

		if (!found) {
			goto invalid;
		}
	}

	if (!strcmp(spec->key, "Animations") && strcmp(v, "0")) {
		goto invalid; // Анимации не поддерживаются
	}

	snprintf(s->values[i], SETTINGS_VALUE, "%s", v);
	s->changed[i] = true;

	if (i == 0) {
		s->ru = !strcmp(v, "ru");
	}

	s->error[0] = 0;
	return 0;
invalid:
	snprintf(s->error, sizeof(s->error), "%s", settings_tr(s->ru, "Invalid value"));
	errno = EINVAL;
	return -1;
}

int settings_load(struct settings *s, const char *root) {
	memset(s, 0, sizeof(*s));

	if (root && strlen(root) >= sizeof(s->root)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	snprintf(s->root, sizeof(s->root), "%s", root ? root : "");
	s->ru = true;

	for (int i = 0; i < setting_count; ++i) {
		snprintf(s->values[i], SETTINGS_VALUE, "%s", setting_specs[i].def);
	}

	for (int f = 0; f < 1; ++f) {
		char path[1024];

		if (make_path(s, files[f], path, sizeof(path))) {
			return -1;
		}

		char *raw = read_file(path, &s->exists[f], &s->hashes[f]);

		if (!raw) {
			goto error;
		}

		char section[80] = "", *save, *line = strtok_r(raw, "\n", &save);

		for (; line; line = strtok_r(NULL, "\n", &save)) {
			char *p = trim(line);

			if (*p == '#' || *p == ';') {
				continue;
			}

			if (*p == '[') {
				char *e = strchr(p, ']');

				if (e) {
					*e = 0;
					snprintf(section, sizeof(section), "%s", p + 1);
				}

				continue;
			}

			char *eq = strchr(p, '=');

			if (!eq) {
				continue;
			}

			*eq = 0;
			int i = find_in(files[f], section, trim(p));

			if (i >= 0) {
				char *v = trim(eq + 1);

				for (char *q = v; *q; ++q) {
					if ((*q == ';' || *q == '#') && q > v && isspace((unsigned char)q[-1])) {
						*q = 0;
						break;
					}
				}

				v = trim(v);

				if (setting_specs[i].type == SET_BOOL) {
					if (!strcasecmp(v, "true") || !strcasecmp(v, "yes")) {
						v = "1";
					} else if (!strcasecmp(v, "false") || !strcasecmp(v, "no")) {
						v = "0";
					}
				}

				settings_set(s, i, v);
			}
			// Историческая совместимость со вторым файлом настроек, в текущем наборе
			// файлов не используется
			if (f == 1 && !strcmp(section, "Desktop") && !strcmp(trim(p), "Theme") &&
			    !s->changed[17]) {
				settings_set(s, 17, trim(eq + 1));
			}
		}

		free(raw);
	}

	memset(s->changed, 0, sizeof(s->changed));
	s->error[0] = 0;
	return 0;
error:
	snprintf(s->error, sizeof(s->error), "%s: %s", settings_tr(s->ru, "Cannot read configuration"),
	         strerror(errno));
	return -1;
}

const char *settings_get(struct settings *s, const char *sec, const char *key) {
	int i = settings_find(sec, key);
	return i >= 0 ? s->values[i] : "";
}

const char *settings_label(struct settings *s, int i) {
	return s->ru ? setting_specs[i].ru : setting_specs[i].en;
}

unsigned settings_color(const char *v) {
	return (unsigned)strtoul(v + 1, NULL, 16);
}

int settings_cycle(struct settings *s, int i) {
	const struct setting_spec *p = &setting_specs[i];

	if (p->type == SET_BOOL) {
		return settings_set(s, i, !strcmp(s->values[i], "0") ? "1" : "0");
	}

	if (!p->choices) {
		return -1;
	}

	char list[128];
	snprintf(list, sizeof(list), "%s", p->choices);
	char *save, *q = strtok_r(list, "|", &save), *first = q;

	while (q) {
		if (!strcmp(q, s->values[i])) {
			q = strtok_r(NULL, "|", &save);
			return settings_set(s, i, q ? q : first);
		}

		q = strtok_r(NULL, "|", &save);
	}

	return settings_set(s, i, first);
}

// Неизменённые строки сохраняются побайтно, включая комментарии, неизвестные ключи и
// CRLF
static char *rewrite(struct settings *s, int f, const char *raw) {
	char *result = NULL;
	size_t length = 0;
	FILE *out = open_memstream(&result, &length);

	if (!out) {
		return NULL;
	}

	bool found[SETTINGS_MAX] = { 0 };
	char section[80] = "";
	const char *p = raw;

	while (*p) {
		const char *end = strchr(p, '\n');
		size_t n = end ? (size_t)(end - p + 1) : strlen(p);
		char *line = strndup(p, n);

		if (!line) {
			fclose(out);
			free(result);
			return NULL;
		}

		char *v = trim(line);
		int i = -1;

		if (*v == '[') {
			char *e = strchr(v, ']');

			if (e) {
				*e = 0;
				snprintf(section, sizeof(section), "%s", v + 1);
			}
		} else if (*v != ';' && *v != '#') {
			char *eq = strchr(v, '=');

			if (eq) {
				*eq = 0;
				i = find_in(files[f], section, trim(v));
			}
		}

		if (i >= 0 && s->changed[i]) {
			const char *eq = memchr(p, '=', n), *comment = NULL;

			for (const char *c = eq + 1; c < p + n; ++c) {
				if ((*c == ';' || *c == '#') && c > eq + 1 && isspace((unsigned char)c[-1])) {
					comment = c;
					break;
				}
			}

			fwrite(p, 1, (size_t)(eq - p + 1), out);
			fprintf(out, " %s", s->values[i]);

			if (comment) {
				fputc(' ', out);
				fwrite(comment, 1, (size_t)(p + n - comment), out);
			} else {
				fputs(n >= 2 && p[n - 2] == '\r' ? "\r\n" : "\n", out);
			}

			found[i] = true;
		} else {
			fwrite(p, 1, n, out);
		}

		free(line);
		p += n;
	}
	// init допускает повторные секции INI, дописываем ключи, сохраняя старые
	// комментарии
	for (int i = 0; i < setting_count; ++i) {
		if (file_id(setting_specs[i].file) == f && s->changed[i] && !found[i]) {
			fprintf(out, "\n[%s]\n%s=%s\n", setting_specs[i].section, setting_specs[i].key,
			        s->values[i]);
		}
	}

	if (fclose(out)) {
		free(result);
		return NULL;
	}

	return result;
}

static int stage(const char *path, const char *data, mode_t mode, char *tmp, size_t cap) {
	if (snprintf(tmp, cap, "%s.tmp.XXXXXX", path) >= (int)cap) {
		errno = ENAMETOOLONG;
		return -1;
	}

	int fd = mkstemp(tmp);

	if (fd < 0) {
		return -1;
	}

	size_t n = strlen(data), pos = 0;
	int result = 0;

	while (pos < n) {
		ssize_t k = write(fd, data + pos, n - pos);

		if (k < 0 && errno == EINTR) {
			continue;
		}

		if (k <= 0) {
			result = -1;
			break;
		}

		pos += (size_t)k;
	}

	if (fchmod(fd, mode) || fsync(fd)) {
		result = -1;
	}

	int saved = errno;

	if (close(fd)) {
		result = -1;
	}

	if (result) {
		unlink(tmp);
	}

	errno = saved;
	return result;
}

int settings_save(struct settings *s) {
	char path[2][1024], tmp[2][1040] = { { 0 } }, backup[1050];
	char *raw[2] = { 0 }, *output[2] = { 0 };
	bool touched[2] = { 0 }, originally_exists[2] = { s->exists[0], s->exists[1] };
	mode_t modes[2] = { 0600, 0600 };
	int done = 0;

	for (int i = 0; i < setting_count; ++i) {
		if (s->changed[i]) {
			touched[file_id(setting_specs[i].file)] = true;
		}
	}

	for (int f = 0; f < 1; ++f) {
		if (touched[f]) {
			if (make_path(s, files[f], path[f], sizeof(path[f]))) {
				goto fail;
			}

			struct stat st;

			if (lstat(path[f], &st) == 0 && !S_ISREG(st.st_mode)) {
				errno = EINVAL;
				goto fail;
			}

			bool exists;
			unsigned long h;
			raw[f] = read_file(path[f], &exists, &h);

			if (!raw[f]) {
				goto fail;
			}

			if (exists != s->exists[f] || h != s->hashes[f]) {
				errno = EBUSY;
				goto fail;
			}

			modes[f] = exists ? (st.st_mode & 0777) : 0600;
			output[f] = rewrite(s, f, raw[f]);

			if (!output[f] || stage(path[f], output[f], exists ? (st.st_mode & 0777) : 0600, tmp[f],
			                        sizeof(tmp[f]))) {
				goto fail;
			}

			if (exists) {
				char bt[1100];
				snprintf(backup, sizeof(backup), "%s.bak", path[f]);

				if (stage(backup, raw[f], st.st_mode & 0777, bt, sizeof(bt))) {
					goto fail;
				}

				if (rename(bt, backup)) {
					unlink(bt);
					goto fail;
				}
			}
		}
	}

	for (int f = 0; f < 1; ++f) {
		if (touched[f]) {
			if (rename(tmp[f], path[f])) {
				goto fail;
			}

			tmp[f][0] = 0;
			done |= 1 << f;
		}
	}

	for (int f = 0; f < 1; ++f) {
		if (touched[f]) {
			s->exists[f] = true;
			s->hashes[f] = hash(output[f], strlen(output[f]));
		}

		free(raw[f]);
		free(output[f]);
	}

	memset(s->changed, 0, sizeof(s->changed));
	s->error[0] = 0;
	return 0;
fail: {
	int saved = errno;

	for (int f = 0; f < 1; ++f) {
		if (tmp[f][0]) {
			unlink(tmp[f]);
		}

		if (done & (1 << f)) {
			char undo[1040];

			if (!originally_exists[f]) {
				unlink(path[f]);
			} else if (!stage(path[f], raw[f], modes[f], undo, sizeof(undo))) {
				if (rename(undo, path[f])) {
					unlink(undo);
				}
			}
		}

		free(raw[f]);
		free(output[f]);
	}

	errno = saved;
	snprintf(s->error, sizeof(s->error), "%s: %s", settings_tr(s->ru, "Cannot save configuration"),
	         strerror(saved));
	return -1;
}
}

unsigned settings_utf8_next(const char **text) {
	const unsigned char *p = (const unsigned char *)*text;
	unsigned c = *p++;

	if (!c) {
		return 0;
	}

	int n = c < 128              ? 0
	        : (c & 0xe0) == 0xc0 ? 1
	        : (c & 0xf0) == 0xe0 ? 2
	        : (c & 0xf8) == 0xf0 ? 3
	                             : -1;
	if (n < 0) {
		*text = (const char *)p;
		return 0xfffd;
	}

	if (n) {
		c &= (1u << (6 - n)) - 1;
	}

	for (int i = 0; i < n; ++i) {
		if ((*p & 0xc0) != 0x80) {
			*text = (const char *)p;
			return 0xfffd;
		}

		c = (c << 6) | (*p++ & 63);
	}

	*text = (const char *)p;
	return c;
}

size_t settings_utf8_width(const char *p) {
	size_t n = 0;

	while (*p) {
		settings_utf8_next(&p);
		++n;
	}

	return n;
}

const char *settings_tr(bool ru, const char *en) {
	static const char *pairs[][2] = {
		{ "Settings", "Настройки" },
		{ "Cannot load wallpaper", "Ошибка загрузки обоев" },
		{ "System settings", "Системные настройки" },
		{ "Save", "Сохранить" },
		{ "Apply", "Применить" },
		{ "Cancel", "Отмена" },
		{ "Defaults", "По умолчанию" },
		{ "Invalid value", "Недопустимое значение" },
		{ "Cannot read configuration", "Ошибка чтения конфигурации" },
		{ "Cannot save configuration", "Ошибка сохранения конфигурации" },
		{ "Configuration saved", "Настройки сохранены" },
		{ "Enter: edit / toggle   S: save   Q: quit", "Enter: изменить   S: сохранить   Q: выход" },
		{ "Discard unsaved settings? (y/n)", "Отменить изменения? (y/n)" },
		{ "Value", "Значение" },
		{ "Reboot required for system changes", "Системные изменения требуют перезагрузки" },
		{ "Start", "Пуск" },
		{ "Overview", "Обзор" },
		{ "Window groups", "Группы окон" },
		{ "Preview / click to open", "Просмотр / нажмите для открытия" },
		{ "Close all windows", "Закрыть все окна" },
		{ "Files", "Файлы" },
		{ "Text Editor", "Редактор" },
		{ "Terminal", "Терминал" },
		{ "Browser (Links)", "Браузер (Links)" },
		{ "Midnight Commander", "Midnight Commander" },
		{ "System Monitor", "Монитор системы" },
		{ "About SynapseOS", "О SynapseOS" },
		{ "Exit session", "Завершить сеанс" },
		{ "Rename group", "Переименовать группу" },
		{ "Split windows", "Разделить окна" },
		{ "Pin group", "Закрепить группу" },
		{ "Unpin group", "Открепить группу" },
		{ "Close group", "Закрыть группу" },
		{ "Open", "Открыть" },
		{ "Save as", "Сохранить как" },
		{ "Cancel", "Отмена" },
		{ "Confirm", "Подтвердить" },
		{ "Untitled", "Без имени" },
		{ "Open path", "Открыть путь" },
		{ "Create folder", "Создать папку" },
		{ "Create file", "Создать файл" },
		{ "Save file as", "Сохранить файл как" },
		{ "Delete permanently?", "Удалить навсегда?" },
		{ "Discard unsaved changes?", "Отменить изменения?" },
		{ "Discard unsaved changes and close?", "Отменить изменения и закрыть?" },
		{ "Up", "Вверх" },
		{ "Refresh", "Обновить" },
		{ "Folder+", "Папка+" },
		{ "File+", "Файл+" },
		{ "Copy", "Копировать" },
		{ "Paste", "Вставить" },
		{ "Delete", "Удалить" },
	};

	if (ru) {
		for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
			if (!strcmp(en, pairs[i][0])) {
				return pairs[i][1];
			}
		}
	}

	return en;
}
