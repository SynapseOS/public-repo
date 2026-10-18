/* Opt-in port test: build with libsqlite3.a, run on a test guest. */
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
static sqlite3 *db;

static void sql(const char *s) {
	char *err = 0;
	int rc = sqlite3_exec(db, s, 0, 0, &err);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL %d: %s\n", rc, err);
		exit(1);
	}
}

int main(int argc, char **argv) {
	sqlite3_stmt *stmt = 0;

	if (argc != 3)
		return 2;
	if (sqlite3_open(argv[2], &db) != SQLITE_OK)
		return 3;
	if (strcmp(argv[1], "init") == 0) {
		sql("CREATE TABLE test(v TEXT);BEGIN;INSERT INTO test VALUES('Привет');COMMIT;BEGIN;INSERT INTO test VALUES('rollback');ROLLBACK;");
	} else if (strcmp(argv[1], "crash") == 0) {
		/* Force dirty pages out of the small cache before an unclean exit.
         * The test runner checks a hot journal and changed on-disk DB. */
		sql("PRAGMA cache_size=5;PRAGMA cache_spill=ON;BEGIN IMMEDIATE;UPDATE test SET v='uncommitted';");

		for (int i = 0; i < 200; i++)
			sql("INSERT INTO test VALUES(printf('%04096d',1));");
		_exit(71); /* Deliberately skip sqlite3_close and rollback. */
	} else if (strcmp(argv[1], "hold") == 0) {
		sql("BEGIN IMMEDIATE;");
		FILE *f = fopen("/tmp/sqlite-ready", "w");

		if (!f)
			return 4;
		fputs("ready", f);
		fclose(f);
		sleep(8);
		sql("ROLLBACK;");
	} else if (strcmp(argv[1], "busy") == 0) {
		int rc = sqlite3_exec(db, "INSERT INTO test VALUES('forbidden');", 0, 0, 0);

		if (rc != SQLITE_BUSY) {
			fprintf(stderr, "expected BUSY, got %d\n", rc);
			return 5;
		}

		puts("BUSY correctly returned");
	} else if (strcmp(argv[1], "verify") == 0) {
		if (sqlite3_prepare_v2(db, "SELECT count(*),max(v) FROM test", -1, &stmt, 0) != SQLITE_OK)
			return 6;
		if (sqlite3_step(stmt) != SQLITE_ROW || sqlite3_column_int(stmt, 0) != 1 ||
		    strcmp((const char *)sqlite3_column_text(stmt, 1), "Привет") != 0)
			return 7;
		sqlite3_finalize(stmt);

		if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, 0) != SQLITE_OK)
			return 8;
		if (sqlite3_step(stmt) != SQLITE_ROW ||
		    strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") != 0)
			return 9;
		sqlite3_finalize(stmt);
		puts("transaction, UTF-8, rollback, reopen and integrity OK");
	} else
		return 10;
	return sqlite3_close(db) != SQLITE_OK;
}
