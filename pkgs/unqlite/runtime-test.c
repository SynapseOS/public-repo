#include <unqlite.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
/* Linked only into this test with --wrap=pwrite. Kill the process after a
 * real database write during commit; journal-only writes must not trigger it. */
static int crash_on_database_write;
static dev_t crash_device;
static ino_t crash_inode;
ssize_t __real_pwrite(int fd, const void *data, size_t n, off_t offset);

ssize_t __wrap_pwrite(int fd, const void *data, size_t n, off_t offset) {
	ssize_t rc = __real_pwrite(fd, data, n, offset);
	struct stat st;

	if (crash_on_database_write && rc > 0 && fstat(fd, &st) == 0 && st.st_dev == crash_device &&
	    st.st_ino == crash_inode)
		_exit(71);
	return rc;
}

static int output(const void *p, unsigned n, void *x) {
	(void)x;
	return fwrite(p, 1, n, stdout) == n ? UNQLITE_OK : UNQLITE_IOERR;
}

static void ok(int rc) {
	if (rc != UNQLITE_OK) {
		fprintf(stderr, "UnQLite rc=%d\n", rc);
		exit(1);
	}
}

int main(int argc, char **argv) {
	if (argc != 3)
		return 2;
	unqlite *db = 0;
	int rc = unqlite_open(&db, argv[2], UNQLITE_OPEN_CREATE);

	if (!strcmp(argv[1], "busy") && rc == UNQLITE_BUSY) {
		puts("BUSY correctly returned");
		return 0;
	}

	ok(rc);
	ok(unqlite_config(db, UNQLITE_CONFIG_DISABLE_AUTO_COMMIT));

	if (!strcmp(argv[1], "init")) {
		ok(unqlite_begin(db));
		ok(unqlite_kv_store(db, "key", 3, "Привет", 12));
		ok(unqlite_commit(db));
		ok(unqlite_begin(db));
		ok(unqlite_kv_store(db, "key", 3, "rollback", 8));
		ok(unqlite_rollback(db));
	} else if (!strcmp(argv[1], "jx9")) {
		unqlite_vm *vm;
		ok(unqlite_compile(db, "print 'Jx9 OK';", -1, &vm));
		ok(unqlite_vm_config(vm, UNQLITE_VM_CONFIG_OUTPUT, output, 0));
		ok(unqlite_vm_exec(vm));
		ok(unqlite_vm_release(vm));
	} else if (!strcmp(argv[1], "verify")) {
		char value[32] = { 0 };
		unqlite_int64 len = sizeof(value);
		ok(unqlite_kv_fetch(db, "key", 3, value, &len));

		if (len != 12 || memcmp(value, "Привет", 12))
			return 3;
		unqlite_kv_cursor *cur = 0;
		int count = 0;
		ok(unqlite_kv_cursor_init(db, &cur));
		ok(unqlite_kv_cursor_first_entry(cur));

		while (unqlite_kv_cursor_valid_entry(cur)) {
			count++;
			rc = unqlite_kv_cursor_next_entry(cur);

			if (rc != UNQLITE_OK && rc != UNQLITE_DONE)
				ok(rc);
		}

		ok(unqlite_kv_cursor_release(db, cur));

		if (count != 1)
			return 7;
		puts("transaction, rollback, UTF-8 and reopen OK");
	} else if (!strcmp(argv[1], "crash")) {
		/* Cache limit is a hint: force an interrupted commit if pages stay pinned. */
		ok(unqlite_config(db, UNQLITE_CONFIG_MAX_PAGE_CACHE, 256));
		ok(unqlite_begin(db));
		ok(unqlite_kv_store(db, "key", 3, "uncommitted", 11));
		char value[8192], key[32];
		memset(value, 'X', sizeof(value));

		for (int i = 0; i < 200; i++) {
			int n = snprintf(key, sizeof(key), "crash-%03d", i);
			ok(unqlite_kv_store(db, key, n, value, sizeof(value)));
		}

		struct stat st;

		if (stat(argv[2], &st) != 0)
			return 8;
		crash_device = st.st_dev;
		crash_inode = st.st_ino;
		crash_on_database_write = 1;
		ok(unqlite_commit(db));
		return 9; /* Wrapper must exit before commit finishes. */
	} else if (!strcmp(argv[1], "hold")) {
		ok(unqlite_begin(db));
		ok(unqlite_kv_store(db, "key", 3, "uncommitted", 11));
		FILE *f = fopen("/tmp/unqlite-ready", "w");

		if (!f)
			return 4;
		fputs("ready", f);
		fclose(f);
		sleep(8);
		ok(unqlite_rollback(db));
	} else if (!strcmp(argv[1], "busy")) {
		rc = unqlite_begin(db);

		if (rc != UNQLITE_BUSY) {
			fprintf(stderr, "expected BUSY got %d\n", rc);
			return 5;
		}

		puts("BUSY correctly returned");
	} else
		return 6;
	ok(unqlite_close(db));
	return 0;
}
