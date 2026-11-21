# Makefile: сборка ncdu для SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI synapse-x86_64-v1
# Подсистема: пользовательские приложения
# Первое изменение: исходная дата не установлена, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)
CURSES := $(abspath ../common/ncurses/install)
UPSTREAM_GENERATED := aclocal.m4 configure config.h.in Makefile.in
.PHONY: all clean
all: $(OUTPUT_DIR)/ncdu
../common/ncurses/install/lib/libncursesw.a:
	$(MAKE) -C ../common/ncurses
.build/Makefile: upstream/configure Makefile ../common/ncurses/install/lib/libncursesw.a
	mkdir -p .build
	cd .build && NCURSES_LIBS="-lncursesw" NCURSES_CFLAGS="-I$(CURSES)/include/ncursesw" ../upstream/configure --host=$(if $(filter arm64 aarch64,$(ARCH)),aarch64,x86_64)-linux-musl --build=$(shell gcc -dumpmachine) CC='$(CC)' CPPFLAGS='-I$(CURSES)/include -I$(CURSES)/include/ncursesw' LDFLAGS='-L$(CURSES)/lib' LIBS='-lncursesw' --disable-nls
$(OUTPUT_DIR)/ncdu: .build/Makefile $(shell find upstream/src -type f \( -name '*.c' -o -name '*.h' \))
	# Используем готовые файлы Autotools независимо от времени checkout
	$(MAKE) -C .build -j4 all-am $(addprefix -o ../upstream/,$(UPSTREAM_GENERATED))
	mkdir -p $(OUTPUT_DIR)
	cp .build/ncdu $@
clean:
	rm -rf .build
	rm -f $(OUTPUT_DIR)/ncdu
