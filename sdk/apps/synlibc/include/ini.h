#ifndef SYN_INI_H
#define SYN_INI_H

#include <synlibc.h>

typedef struct ini_entry {
	char *key;
	char *value;
	struct ini_entry *next;
} ini_entry_t;

typedef struct ini_section {
	char *name;
	ini_entry_t *entries;
	struct ini_section *next;
} ini_section_t;

typedef struct ini_file {
	ini_section_t *sections;
	char *raw;
	size_t raw_size;
} ini_file_t;

ini_file_t *ini_parse(const char *path);
ini_file_t *ini_parse_buffer(char *buf, size_t size);
const char *ini_get(const ini_file_t *ini, const char *section, const char *key);
int ini_get_int(const ini_file_t *ini, const char *section, const char *key, int def);
int ini_get_bool(const ini_file_t *ini, const char *section, const char *key, int def);
void ini_free(ini_file_t *ini);
void ini_dump(const ini_file_t *ini);

#endif