/* SynapseOS UnQLite command line frontend. */
#include "unqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int output(const void *data, unsigned int size, void *unused) {
	(void)unused;
	return fwrite(data, 1, size, stdout) == size ? UNQLITE_OK : UNQLITE_IOERR;
}

static int error(unqlite *db, int rc) {
	const char *message = 0;
	int length = 0;

	if (db)
		unqlite_config(db, UNQLITE_CONFIG_ERR_LOG, &message, &length);
	fprintf(stderr, "unqlite: error %d%s%.*s\n", rc, length ? ": " : "", length,
	        message ? message : "");
	return rc == UNQLITE_NOTFOUND ? 2 : 1;
}

int main(int argc, char **argv) {
	unqlite *db = 0;
	unqlite_kv_cursor *cursor = 0;
	int rc, writing;

	if (argc == 2 && !strcmp(argv[1], "--version")) {
		puts("UnQLite " UNQLITE_VERSION " (SynapseOS)");
		return 0;
	}

	if (argc < 3 || (!strcmp(argv[2], "set")                                   ? argc != 5
	                 : (!strcmp(argv[2], "get") || !strcmp(argv[2], "delete")) ? argc != 4
	                 : !strcmp(argv[2], "list")                                ? argc != 3
	                                                                           : 1)) {
		fputs("Usage: unqlite DATABASE {set KEY VALUE|get KEY|delete KEY|list}\n", stderr);
		return 1;
	}

	writing = !strcmp(argv[2], "set") || !strcmp(argv[2], "delete");
	rc = unqlite_open(&db, argv[1], writing ? UNQLITE_OPEN_CREATE : UNQLITE_OPEN_READONLY);

	if (rc != UNQLITE_OK)
		return error(db, rc);
	/* Explicit transactions: never commit an unsuccessful modification on close. */
	unqlite_config(db, UNQLITE_CONFIG_DISABLE_AUTO_COMMIT);

	if (writing) {
		rc = unqlite_begin(db);

		if (rc == UNQLITE_OK) {
			if (!strcmp(argv[2], "set"))
				rc = unqlite_kv_store(db, argv[3], -1, argv[4], strlen(argv[4]));
			else
				rc = unqlite_kv_delete(db, argv[3], -1);
		}

		if (rc == UNQLITE_OK)
			rc = unqlite_commit(db);
		if (rc != UNQLITE_OK)
			unqlite_rollback(db);
	} else if (!strcmp(argv[2], "get")) {
		rc = unqlite_kv_fetch_callback(db, argv[3], -1, output, 0);

		if (rc == UNQLITE_OK && putchar('\n') == EOF)
			rc = UNQLITE_IOERR;
	} else {
		rc = unqlite_kv_cursor_init(db, &cursor);

		if (rc == UNQLITE_OK) {
			rc = unqlite_kv_cursor_first_entry(cursor);

			if (rc == UNQLITE_DONE || rc == UNQLITE_NOTFOUND)
				rc = UNQLITE_OK;
			while (rc == UNQLITE_OK && unqlite_kv_cursor_valid_entry(cursor)) {
				rc = unqlite_kv_cursor_key_callback(cursor, output, 0);

				if (rc == UNQLITE_OK && putchar('\t') == EOF)
					rc = UNQLITE_IOERR;
				if (rc == UNQLITE_OK)
					rc = unqlite_kv_cursor_data_callback(cursor, output, 0);
				if (rc == UNQLITE_OK && putchar('\n') == EOF)
					rc = UNQLITE_IOERR;
				if (rc == UNQLITE_OK) {
					rc = unqlite_kv_cursor_next_entry(cursor);

					if (rc == UNQLITE_DONE)
						rc = UNQLITE_OK;
				}
			}

			unqlite_kv_cursor_release(db, cursor);
		}
	}

	if (fflush(stdout) != 0 && rc == UNQLITE_OK)
		rc = UNQLITE_IOERR;
	int status = rc == UNQLITE_OK ? 0 : error(db, rc);
	rc = unqlite_close(db);
	return status ? status : rc == UNQLITE_OK ? 0 : error(0, rc);
}
