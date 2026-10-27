AM_CPPFLAGS=-I$(srcdir)/deps
bin_PROGRAMS=ncdu

ncdu_SOURCES=\
	src/browser.c\
	src/delete.c\
	src/dirlist.c\
	src/dir_common.c\
	src/dir_export.c\
	src/dir_import.c\
	src/dir_mem.c\
	src/dir_scan.c\
	src/exclude.c\
	src/help.c\
	src/shell.c\
	src/quit.c\
	src/main.c\
	src/path.c\
	src/util.c\
	deps/strnatcmp.c


noinst_HEADERS=\
	deps/khashl.h\
	deps/strnatcmp.h\
	src/browser.h\
	src/delete.h\
	src/dir.h\
	src/dirlist.h\
	src/exclude.h\
	src/global.h\
	src/help.h\
	src/shell.h\
	src/quit.h\
	src/path.h\
	src/util.h


man_MANS=ncdu.1
EXTRA_DIST=ncdu.1

# This target exists more for documentation purposes than actual use; some
# dependencies have minor ncdu-specific changes.
update-deps:
	wget -q https://raw.github.com/attractivechaos/klib/master/khashl.h -O "$(srcdir)/deps/khashl.h"
	wget -q https://raw.githubusercontent.com/sourcefrog/natsort/master/strnatcmp.h -O "$(srcdir)/deps/strnatcmp.h"
	wget -q https://raw.githubusercontent.com/sourcefrog/natsort/master/strnatcmp.c -O "$(srcdir)/deps/strnatcmp.c"
