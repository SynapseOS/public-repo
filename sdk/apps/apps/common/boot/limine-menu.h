// limine-menu.h: Загрузочные записи Limine
// Автор: Елчинян Арен Артурович, 2026
// Замена только помеченного блока с сохранением пользовательских записей
// Подсистема: установка и начальная настройка системы
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 14.10.2026

#ifndef SYNAPSE_LIMINE_MENU_H
#define SYNAPSE_LIMINE_MENU_H
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
// Сборщик образов и setup изменяют только помеченный блок
#define SESSION_BEGIN "# SYNAPSE SESSION MENU BEGIN\n"
#define SESSION_END "# SYNAPSE SESSION MENU END\n"

static char *limine_session_menu(const char *input, int graphical, size_t *length) {
	size_t n = strlen(input);
	char *clean = calloc(1, n + 2);

	if (!clean) {
		return NULL;
	}

	size_t used = 0;
	int managed = 0;

	for (const char *p = input; *p;) {
		const char *end = strchr(p, '\n');
		size_t count = end ? (size_t)(end + 1 - p) : strlen(p);

		if (!strncmp(p, SESSION_BEGIN, strlen(SESSION_BEGIN))) {
			managed = 1;
		} else if (!strncmp(p, SESSION_END, strlen(SESSION_END))) {
			managed = 0;
		} else if (!managed) {
			memcpy(clean + used, p, count);
			used += count;
		}

		p += count;
	}
	// Неполный блок не должен приводить к потере пользовательских записей
	if (managed) {
		free(clean);
		return NULL;
	}

	if (!graphical) {
		*length = used;
		return clean;
	}

	char *first = clean;

	while (*first && !(*first == '/' && (first == clean || first[-1] == '\n'))) {
		first++;
	}

	char *body = strchr(first, '\n');

	if (!*first || !body) {
		free(clean);
		return NULL;
	}

	body++;
	char *next = body;

	while (*next && !(*next == '/' && next[-1] == '\n')) {
		next++;
	}

	int limine = 0;

	for (char *p = body; p < next;) {
		char *end = memchr(p, '\n', (size_t)(next - p));

		if (!end) {
			end = next;
		}

		char *trim = p;

		while (trim < end && (*trim == ' ' || *trim == '\t')) {
			trim++;
		}

		if ((size_t)(end - trim) >= 16 && !memcmp(trim, "protocol: limine", 16)) {
			limine = 1;
		}

		p = end < next ? end + 1 : next;
	}

	if (!limine) {
		free(clean);
		return NULL;
	}

	size_t capacity = used * 3 + 1024;
	char *out = calloc(1, capacity);

	if (!out) {
		free(clean);
		return NULL;
	}

	size_t at = (size_t)(next - clean);
	memcpy(out, clean, at);

	if (at && out[at - 1] != '\n') {
		out[at++] = '\n';
	}

	at += snprintf(out + at, capacity - at, "%s", SESSION_BEGIN);
	const char *names[] = { "Graphical session", "Terminal" };
	const char *modes[] = { "desktop", "console" };

	for (int i = 0; i < 2; i++) {
		at += snprintf(out + at, capacity - at, "/SynapseOS (%s)\n", names[i]);
		int cmdline = 0;

		for (char *p = body; p < next;) {
			char *end = memchr(p, '\n', (size_t)(next - p));

			if (!end) {
				end = next;
			}

			char *trim = p;

			while (trim < end && (*trim == ' ' || *trim == '\t')) {
				trim++;
			}

			if ((size_t)(end - trim) >= 8 && !memcmp(trim, "cmdline:", 8)) {
				cmdline = 1;
				at += snprintf(out + at, capacity - at, "    cmdline:");

				for (char *t = trim + 8; t < end;) {
					while (t < end && (*t == ' ' || *t == '\t' || *t == '\r')) {
						t++;
					}

					char *token = t;

					while (t < end && *t != ' ' && *t != '\t' && *t != '\r') {
						t++;
					}

					size_t size = (size_t)(t - token);

					if (size && !(size >= 8 && !memcmp(token, "session=", 8))) {
						at += snprintf(out + at, capacity - at, " %.*s", (int)size, token);
					}
				}

				at += snprintf(out + at, capacity - at, " session=%s\n", modes[i]);
			} else if (*trim && trim < end && strncmp(trim, "comment:", 8) && *trim != '#') {
				size_t size = (size_t)(end - p);
				memcpy(out + at, p, size);
				at += size;
				out[at++] = '\n';
			}

			p = end < next ? end + 1 : next;
		}

		if (!cmdline) {
			at += snprintf(out + at, capacity - at, "    cmdline: session=%s\n", modes[i]);
		}

		out[at++] = '\n';
	}

	at += snprintf(out + at, capacity - at, "%s", SESSION_END);
	memcpy(out + at, next, strlen(next));
	at += strlen(next);
	*length = at;
	free(clean);
	return out;
}
#endif
