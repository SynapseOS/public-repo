.SUFFIXES:

ARCH := x86_64

CC := gcc
AR := ar
LD := ld
NASM := nasm

# ============================================================
# Пути к библиотекам
# ============================================================
MUSL_DIR    := $(CURDIR)/../musl
MUSL_INC    := $(MUSL_DIR)/include
MUSL_LIB    := $(MUSL_DIR)/lib

SYNLIBC_DIR := $(CURDIR)/../synlibc
SYNLIBC_INC := $(SYNLIBC_DIR)/include

# ============================================================
# Общие флаги компиляции
# ============================================================
COMMON_CFLAGS := -O0 -pipe -std=gnu11 -w \
    -ffreestanding -fno-stack-protector -fno-stack-check \
    -fno-lto -fno-plt -ffunction-sections -fdata-sections

COMMON_CPPFLAGS :=  \
    -nostdinc \
    -I$(MUSL_DIR)/obj/include \
    -I$(MUSL_INC) \
    -I$(MUSL_DIR)/arch/x86_64 \
    -I$(MUSL_DIR)/arch/generic


ifeq ($(ARCH),x86_64)
    COMMON_CFLAGS += -m64 -march=x86-64 -mabi=sysv \
        -mno-80387 -mno-mmx -mno-red-zone \
        -mcmodel=small -fno-pie -no-pie
    COMMON_LDFLAGS := -m elf_x86_64 -nostdlib \
        -z max-page-size=0x1000 --gc-sections \
        -no-pie -static
    NASMFLAGS := -g -F dwarf -f elf64 -Wall
endif

# ============================================================
# Экспорт переменных для дочерних Makefile
# ============================================================
export ARCH CC AR LD NASM NASMFLAGS
export MUSL_DIR MUSL_INC MUSL_LIB
export SYNLIBC_DIR SYNLIBC_INC
export COMMON_CFLAGS COMMON_CPPFLAGS COMMON_LDFLAGS
export OUTPUT_DIR := $(CURDIR)/apps-bin
export COMMON_DIR := $(CURDIR)/common

# ============================================================
# Обнаружение подпапок с Makefile
# ============================================================
APP_DIRS := $(filter-out dropbear/,$(dir $(wildcard */Makefile))) dropbear/
APP_DIRS := $(APP_DIRS:./=)

.PHONY: all clean $(APP_DIRS)

all: $(APP_DIRS)

# Build the shared curses library once before parallel application builds.
.PHONY: curses
curses:
	$(MAKE) -C common/ncurses
nano/ ncdu/ top/: curses

.PHONY: musl-shared
musl-shared:
	$(MAKE) -C $(MUSL_DIR) -j4 lib/libc.so
lua/: musl-shared

.PHONY: tls
tls:
	$(MAKE) -C common/mbedtls
https/ httpsd/ setup/ links/: tls

$(APP_DIRS):
	@echo "=== Building $@ ==="
	$(MAKE) -C $@ $(if $(filter dropbear/,$@),-f synapse.mk)

clean:
	@for d in $(APP_DIRS); do \
		if [ "$$d" = dropbear/ ]; then \
			$(MAKE) -C $$d -f synapse.mk clean; \
		else \
			$(MAKE) -C $$d clean; \
		fi; \
	done
	rm -rf $(OUTPUT_DIR)
