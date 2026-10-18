// settings.h: Общие типы системных настроек
// Автор: Елчинян Арен Артурович, 2026
// Поля INI, результаты проверки и состояние редактора
// Подсистема: пользовательские системные утилиты
// Создан: 12.10.2026
// Последнее изменение: 14.10.2026

#ifndef SYN_SETTINGS_H
#define SYN_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
#define SETTINGS_MAX 64
#define SETTINGS_VALUE 256

struct setting_spec {
	const char *file, *section, *key, *en, *ru, *def, *choices, *effect;
	int type, min, max;
};

enum { SET_TEXT, SET_BOOL, SET_CHOICE, SET_INT, SET_COLOR, SET_PATH };
extern const struct setting_spec setting_specs[];
extern const int setting_count;

struct settings {
	char root[512], values[SETTINGS_MAX][SETTINGS_VALUE];
	unsigned long hashes[3];
	bool exists[3], changed[SETTINGS_MAX], ru;
	char error[256];
};

int settings_load(struct settings *s, const char *root);
int settings_set(struct settings *s, int index, const char *value);
int settings_save(struct settings *s);
int settings_find(const char *section, const char *key);
const char *settings_get(struct settings *s, const char *section, const char *key);
const char *settings_label(struct settings *s, int index);
const char *settings_tr(bool ru, const char *en);
int settings_cycle(struct settings *s, int index);
unsigned settings_color(const char *value);
unsigned settings_utf8_next(const char **text);
size_t settings_utf8_width(const char *text);
#endif
