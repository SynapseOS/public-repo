// json.h: Интерфейс JSON-парсера
// Автор: Елчинян Арен Артурович, 2026
// Типы токенов, поиск полей и извлечение строк
// Подсистема: пользовательские системные утилиты
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#ifndef QGA_JSON_H
#define QGA_JSON_H
#include <stddef.h>
#include <stdint.h>

#ifndef JSON_TOKENS
#define JSON_TOKENS 512
#endif
enum json_type { J_OBJECT, J_ARRAY, J_STRING, J_NUMBER, J_BOOL, J_NULL };

struct json_token {
	enum json_type type;
	size_t start, end;
	int next, children;
};

struct json {
	const char *text;
	size_t len, pos;
	int count;
	struct json_token t[JSON_TOKENS];
};

int json_parse(struct json *j, const char *text, size_t len);
int json_get(const struct json *j, int object, const char *key);
int json_string(const struct json *j, int token, char *out, size_t capacity);
int json_integer(const struct json *j, int token, int64_t *out);
int json_boolean(const struct json *j, int token, int *out);
#endif
