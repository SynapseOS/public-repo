// setup.h: Общие типы и функции setup
// Автор: Елчинян Арен Артурович, 2026
// Структуры каталога, плана установки и состояния транзакции
// Подсистема: пакетный менеджер setup
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#ifndef SYNAPSE_SETUP_H
#define SYNAPSE_SETUP_H
#define _GNU_SOURCE
#include "../qemu-ga/json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct download_progress {
	long long start, last;
	uint64_t total;
	int tty;
};

void progress_start(struct download_progress *, uint64_t);
void progress_update(struct download_progress *, uint64_t, int);
#define ABI "synapse-x86_64-v1"
#define MAXPKG 512
#define MAXFILES 512
#define MAXDEP 32
#define MAXREPO 16
#define META_LIMIT (4 * 1024 * 1024)
#define PACKAGE_LIMIT (256 * 1024 * 1024)

struct file {
	char path[512], hash[65], target[512];
	uint64_t size;
	unsigned mode;
	int config, type;
};

struct package {
	char name[64], version[64], description[256], kind[32], arch[32], abi[64];
	char dep[MAXDEP][192], conflict[MAXDEP][64];
	int ndep, nconf, nfile;
	struct file *files;
	char *manifest;
	size_t manifest_len;
	int repo, explicit, installed, reinstall;
	char hash[65], url[128];
	uint64_t size;
};

struct repo {
	char id[64], url[1024], key[65], prefix[64];
	char token_file[512];
	int priority, official;
	uint64_t minimum, generation;
	time_t expires;
};
extern struct package *available[MAXPKG], *installed[MAXPKG], *chosen[MAXPKG];
extern int navailable, ninstalled, nchosen, nrepo, dry_run, yes;
extern struct repo repos[MAXREPO];
extern jmp_buf failure;
extern int transaction_started;
extern const char *root;
void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void path(char *out, const char *relative);
void join_path(char *out, size_t cap, const char *parent, const char *leaf);
void mkdirs(const char *p);
void sync_parent(const char *p);
void write_bytes(const char *p, const void *data, size_t len, unsigned mode);
char *read_bytes(const char *p, size_t limit, size_t *len);
void copy_file(const char *src, const char *dest, unsigned mode, int exclusive);
void sha_file(const char *p, char out[65]);
void sha_bytes(const void *p, size_t n, char out[65]);
int unhex(const char *s, unsigned char *out, size_t len);
int safe_path(const char *s);
int valid_name(const char *s);
struct json *parse_json(const char *s, size_t len);
void object_fields(struct json *j, int t, const char *allowed);
void jstr(struct json *j, int t, const char *key, char *out, size_t cap);
uint64_t jnum(struct json *j, int t, const char *key);
struct package *parse_package(struct json *j, int t);
void load_state(void);
void load_repos(int update);
void plus_command(int argc, char **argv);
void auth_command(int argc, char **argv);
void private_fetch(const char *url, const char *dest, size_t limit, const char *token_file);
void fetch(const char *url, const char *dest, size_t limit);
int version_cmp(const char *a, const char *b);
int satisfies(const char *version, const char *constraint);
void solve(int argc, char **argv, int upgrade);
void plan_remove(int argc, char **argv);
void transact(void);
void recover(void);
void verify(int argc, char **argv);
void serialize_db(const char *dest);
void extract(struct package *p, const char *archive, const char *stage);
int package_same(struct package *a, struct package *b);
#endif
