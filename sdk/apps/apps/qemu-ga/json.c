// json.c: Разбор JSON для QGA и setup
// Автор: Елчинян Арен Артурович, 2026
// Проверка сообщений с ограниченным числом токенов
// Подсистема: пользовательские системные утилиты
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

// JSON-парсер хранит смещения токенов, чтобы передавать QMP id без преобразования
#include "json.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void whitespace(struct json *j) {
	while (j->pos < j->len && strchr(" \r\n\t", j->text[j->pos])) {
		j->pos++;
	}
}

static int hex4(const char *p, unsigned *v) {
	*v = 0;

	for (int i = 0; i < 4; ++i) {
		unsigned c = (unsigned char)p[i], n;

		if (c >= '0' && c <= '9') {
			n = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			n = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			n = c - 'A' + 10;
		} else {
			return -1;
		}

		*v = (*v << 4) | n;
	}

	return 0;
}

static int value(struct json *j, unsigned depth) {
	whitespace(j);

	if (depth > 32 || j->pos == j->len || j->count == JSON_TOKENS) {
		return -1;
	}

	int n = j->count++;
	struct json_token *t = &j->t[n];
	t->start = j->pos;
	t->children = 0;
	char c = j->text[j->pos++];

	if (c == '{' || c == '[') {
		t->type = c == '{' ? J_OBJECT : J_ARRAY;
		char end = c == '{' ? '}' : ']';
		whitespace(j);

		if (j->pos < j->len && j->text[j->pos] == end) {
			j->pos++;
		} else {
			for (;;) {
				int key = value(j, depth + 1);

				if (key < 0) {
					return -1;
				}

				t->children++;

				if (c == '{') {
					if (j->t[key].type != J_STRING) {
						return -1;
					}

					whitespace(j);

					if (j->pos == j->len || j->text[j->pos++] != ':') {
						return -1;
					}

					if (value(j, depth + 1) < 0) {
						return -1;
					}

					t->children++;
				}

				whitespace(j);

				if (j->pos == j->len) {
					return -1;
				}

				char sep = j->text[j->pos++];

				if (sep == end) {
					break;
				}

				if (sep != ',') {
					return -1;
				}
			}
		}
	} else if (c == '"') {
		t->type = J_STRING;
		int closed = 0;

		while (j->pos < j->len) {
			unsigned char ch = j->text[j->pos++];

			if (ch == '"') {
				closed = 1;
				break;
			}

			if (ch < 32) {
				return -1;
			}

			if (ch == '\\') {
				if (j->pos == j->len) {
					return -1;
				}

				ch = j->text[j->pos++];

				if (ch == 'u') {
					unsigned u;

					if (j->len - j->pos < 4 || hex4(j->text + j->pos, &u)) {
						return -1;
					}

					j->pos += 4;

					if (u >= 0xd800 && u <= 0xdbff) {
						unsigned low;

						if (j->len - j->pos < 6 || j->text[j->pos] != '\\' ||
						    j->text[j->pos + 1] != 'u' || hex4(j->text + j->pos + 2, &low) ||
						    low < 0xdc00 || low > 0xdfff) {
							return -1;
						}

						j->pos += 6;
					} else if (u >= 0xdc00 && u <= 0xdfff) {
						return -1;
					}
				} else if (!strchr("\"\\/bfnrt", ch)) {
					return -1;
				}
			}
		}

		if (!closed) {
			return -1;
		}
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		t->type = J_NUMBER;
		size_t p = t->start;

		if (j->text[p] == '-') {
			p++;
		}

		if (p == j->len || !isdigit((unsigned char)j->text[p])) {
			return -1;
		}

		if (j->text[p] == '0') {
			p++;
		} else {
			while (p < j->len && isdigit((unsigned char)j->text[p])) {
				p++;
			}
		}

		if (p < j->len && j->text[p] == '.') {
			p++;
			size_t first = p;

			while (p < j->len && isdigit((unsigned char)j->text[p])) {
				p++;
			}

			if (p == first) {
				return -1;
			}
		}

		if (p < j->len && (j->text[p] == 'e' || j->text[p] == 'E')) {
			p++;

			if (p < j->len && (j->text[p] == '+' || j->text[p] == '-')) {
				p++;
			}

			size_t first = p;

			while (p < j->len && isdigit((unsigned char)j->text[p])) {
				p++;
			}

			if (p == first) {
				return -1;
			}
		}

		j->pos = p;
	} else {
		const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;

		if (!literal) {
			return -1;
		}

		size_t len = strlen(literal);

		if (j->len - t->start < len || memcmp(j->text + t->start, literal, len)) {
			return -1;
		}

		t->type = c == 'n' ? J_NULL : J_BOOL;
		j->pos = t->start + len;
	}

	t->end = j->pos;
	t->next = j->count;
	return n;
}

int json_parse(struct json *j, const char *text, size_t len) {
	memset(j, 0, sizeof(*j));
	j->text = text;
	j->len = len;

	if (value(j, 0) < 0) {
		return -1;
	}

	whitespace(j);
	return j->pos == len ? 0 : -1;
}

int json_string(const struct json *j, int n, char *out, size_t cap) {
	if (n < 0 || n >= j->count || j->t[n].type != J_STRING || !cap) {
		return -1;
	}

	size_t len = 0;

	for (size_t p = j->t[n].start + 1; p < j->t[n].end - 1; ++p) {
		unsigned c = (unsigned char)j->text[p];

		if (c == '\\') {
			c = (unsigned char)j->text[++p];

			if (c == 'u') {
				if (hex4(j->text + p + 1, &c)) {
					return -1;
				}

				p += 4;

				if (c >= 0xd800 && c <= 0xdbff) {
					unsigned low;

					if (hex4(j->text + p + 3, &low)) {
						return -1;
					}

					c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
					p += 6;
				}

				if (!c) {
					return -1; // Строки C не могут содержать нулевой байт внутри
					    // значения
				}

				unsigned char bytes[4];
				size_t count;

				if (c < 0x80) {
					bytes[0] = c;
					count = 1;
				} else if (c < 0x800) {
					bytes[0] = 0xc0 | (c >> 6);
					bytes[1] = 0x80 | (c & 63);
					count = 2;
				} else if (c < 0x10000) {
					bytes[0] = 0xe0 | (c >> 12);
					bytes[1] = 0x80 | ((c >> 6) & 63);
					bytes[2] = 0x80 | (c & 63);
					count = 3;
				} else {
					bytes[0] = 0xf0 | (c >> 18);
					bytes[1] = 0x80 | ((c >> 12) & 63);
					bytes[2] = 0x80 | ((c >> 6) & 63);
					bytes[3] = 0x80 | (c & 63);
					count = 4;
				}

				if (len + count >= cap) {
					return -1;
				}

				memcpy(out + len, bytes, count);
				len += count;
				continue;
			}

			switch (c) {
				case 'b':
					c = '\b';
					break;
				case 'f':
					c = '\f';
					break;
				case 'n':
					c = '\n';
					break;
				case 'r':
					c = '\r';
					break;
				case 't':
					c = '\t';
					break;
			}
		}

		if (len + 1 >= cap) {
			return -1;
		}

		out[len++] = c;
	}

	out[len] = 0;
	return (int)len;
}

int json_get(const struct json *j, int n, const char *key) {
	if (n < 0 || n >= j->count || j->t[n].type != J_OBJECT) {
		return -1;
	}

	char buf[128];
	int result = -1;

	for (int i = n + 1; i < j->t[n].next;) {
		int v = i + 1;

		if (json_string(j, i, buf, sizeof(buf)) >= 0 && !strcmp(buf, key)) {
			if (result >= 0) {
				return -1; // Повтор поля делает значение неоднозначным
			}

			result = v;
		}

		i = j->t[v].next;
	}

	return result;
}

int json_integer(const struct json *j, int n, int64_t *out) {
	if (n < 0 || n >= j->count || j->t[n].type != J_NUMBER) {
		return -1;
	}

	size_t len = j->t[n].end - j->t[n].start;
	char buf[32], *end;

	if (len >= sizeof(buf)) {
		return -1;
	}

	memcpy(buf, j->text + j->t[n].start, len);
	buf[len] = 0;
	errno = 0;
	long long num = strtoll(buf, &end, 10);

	if (errno || *end) {
		return -1;
	}

	*out = num;
	return 0;
}

int json_boolean(const struct json *j, int n, int *out) {
	if (n < 0 || n >= j->count || j->t[n].type != J_BOOL) {
		return -1;
	}

	*out = j->text[j->t[n].start] == 't';
	return 0;
}
