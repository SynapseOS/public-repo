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
export OUTPUT_DIR := $(CURDIR)/libs-bin
export COMMON_DIR := $(CURDIR)/../apps/common

# ============================================================
# Обнаружение подпапок с Makefile
# ============================================================
LIB_DIRS := $(dir $(wildcard */Makefile))
LIB_DIRS := $(LIB_DIRS:./=)

.PHONY: all clean $(LIB_DIRS)

all: $(LIB_DIRS)

$(LIB_DIRS):
	@echo "=== Building library $@ ==="
	$(MAKE) -C $@

clean:
	@for d in $(LIB_DIRS); do \
		$(MAKE) -C $$d clean; \
	done
	rm -rf $(OUTPUT_DIR)