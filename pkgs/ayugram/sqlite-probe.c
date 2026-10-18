// sqlite-probe.c: проверить встроенный SQLite AyuGram на отдельной временной базе
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: проверки графических пакетов
// Первое изменение: 19.10.2026, последнее изменение: 19.10.2026
// Статус доступа: общедоступный, MIT
#define _GNU_SOURCE
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Остановить проверку, оставив её временную базу для разбора ошибки
static void require(int condition, const char *operation) {
	if (condition == 0) {
		fprintf(stderr, "AYUGRAM_SQLITE_FAIL %s\n", operation);
		exit(1);
	}
}

// Выполнить запрос без доступа к пользовательской базе
static void execute(sqlite3 *database, const char *statement) {
	require(sqlite3_exec(database, statement, NULL, NULL, NULL) == SQLITE_OK, statement);
}

// Проверить транзакции, повторное открытие и запрет одновременной записи
int main(void) {
	alarm(15);
	sqlite3_vfs *vfs = sqlite3_vfs_find(NULL);
	require(vfs != NULL && strcmp(vfs->zName, "unix-dotfile") == 0, "default VFS");

	char directory[] = "/tmp/ayugram-sqlite-XXXXXX";
	require(mkdtemp(directory) != NULL, "temporary directory");
	char path[128];
	int length = snprintf(path, sizeof(path), "%s/test.db", directory);
	require(length > 0 && (size_t)length < sizeof(path), "database path");

	sqlite3 *database = NULL;
	require(sqlite3_open(path, &database) == SQLITE_OK && database != NULL, "open database");
	execute(database, "CREATE TABLE test(v TEXT);BEGIN;INSERT INTO test VALUES('Привет');COMMIT;");
	execute(database, "BEGIN;INSERT INTO test VALUES('rollback');ROLLBACK;");
	execute(database, "BEGIN IMMEDIATE;");

	pid_t child = fork( );
	require(child >= 0, "fork");
	if (child == 0) {
		alarm(5);
		sqlite3 *other = NULL;
		if (sqlite3_open(path, &other) != SQLITE_OK || other == NULL) {
			_exit(2);
		}

		int result = sqlite3_exec(other, "INSERT INTO test VALUES('forbidden');", NULL, NULL, NULL);
		int closed = sqlite3_close(other);
		if (result != SQLITE_BUSY || closed != SQLITE_OK) {
			_exit(3);
		}

		_exit(0);
	}

	int status = 0;
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	        "writer exclusion");
	execute(database, "ROLLBACK;");
	require(sqlite3_close(database) == SQLITE_OK, "close database");
	database = NULL;
	require(sqlite3_open(path, &database) == SQLITE_OK && database != NULL, "reopen database");

	sqlite3_stmt *statement = NULL;
	require(sqlite3_prepare_v2(database, "SELECT count(*),max(v) FROM test", -1, &statement,
	                           NULL) == SQLITE_OK &&
	            statement != NULL,
	        "prepare verification");
	require(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1,
	        "transaction and rollback");
	const unsigned char *text = sqlite3_column_text(statement, 1);
	require(text != NULL && strcmp((const char *)text, "Привет") == 0, "UTF-8 after reopen");
	require(sqlite3_finalize(statement) == SQLITE_OK, "finalize verification");
	require(sqlite3_close(database) == SQLITE_OK, "final close");
	require(unlink(path) == 0 && rmdir(directory) == 0, "remove test database");
	alarm(0);
	puts("AYUGRAM_SQLITE_PASS");
	return 0;
}
