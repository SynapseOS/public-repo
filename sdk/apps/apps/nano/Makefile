# Makefile: сборка nano для SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI synapse-x86_64-v1
# Подсистема: пользовательские приложения
# Первое изменение: исходная дата не установлена, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)
CURSES := $(abspath ../common/ncurses/install)
.PHONY: all clean
all: $(OUTPUT_DIR)/nano
../common/ncurses/install/lib/libncursesw.a:
	$(MAKE) -C ../common/ncurses
.build/Makefile: upstream/configure Makefile ../common/ncurses/install/lib/libncursesw.a
	mkdir -p .build
	cd .build && ../upstream/configure --host=x86_64-linux-musl --build=x86_64-linux-gnu CC='$(CC)' CPPFLAGS='-D__synapseos__ -I$(CURSES)/include -I$(CURSES)/include/ncursesw' LDFLAGS='-L$(CURSES)/lib' LIBS='-lncursesw' --disable-maintainer-mode --disable-nls --disable-libmagic --disable-speller --disable-browser --without-slang
$(OUTPUT_DIR)/nano: .build/Makefile $(shell find upstream/src -type f \( -name '*.c' -o -name '*.h' \))
	$(MAKE) -C .build/lib -j4
	$(MAKE) -C .build/src -j4
	mkdir -p $(OUTPUT_DIR)
	cp .build/src/nano $@
clean:
	rm -rf .build
	rm -f $(OUTPUT_DIR)/nano
