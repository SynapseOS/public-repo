OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)
CURSES := $(abspath ../common/ncurses/install)
.PHONY: all clean
all: $(OUTPUT_DIR)/top
../common/ncurses/install/lib/libncursesw.a:
	$(MAKE) -C ../common/ncurses
$(OUTPUT_DIR)/top: top.c ../../../include/lumine/process.h ../common/ncurses/install/lib/libncursesw.a
	mkdir -p $(OUTPUT_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Wl,--build-id=none -I$(CURSES)/include -I$(CURSES)/include/ncursesw top.c -L$(CURSES)/lib -lncursesw -o $@
clean:
	rm -f $(OUTPUT_DIR)/top
