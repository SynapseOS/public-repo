# apps/ubase/Makefile
# Отдельные бинарники ubase для Синапс ОС (musl)

LIBC := musl

# ============================================================
# Список утилит для сборки
# ============================================================
# Build every utility whose source is present in this checkout.
BIN := $(sort $(basename $(notdir $(wildcard *.c))))

# ============================================================
# Исходники libutil
# ============================================================
LIBUTILSRC := \
	libutil/agetcwd.c        libutil/agetline.c       \
	libutil/apathmax.c       libutil/concat.c         \
	libutil/ealloc.c         libutil/eprintf.c        \
	libutil/estrtol.c        libutil/estrtoul.c       \
	libutil/explicit_bzero.c libutil/passwd.c         \
	libutil/proc.c           libutil/putword.c        \
	libutil/recurse.c        libutil/strlcat.c        \
	libutil/strlcpy.c        libutil/strtonum.c       \
	libutil/tty.c

HDR := arg.h config.h passwd.h proc.h queue.h reboot.h rtc.h text.h util.h

OBJ_DIR     := obj
LIBUTILOBJ  := $(patsubst libutil/%.c,$(OBJ_DIR)/libutil/%.o,$(LIBUTILSRC))
LIBUTIL_A   := $(OBJ_DIR)/libutil.a

# Целевые бинарники в apps-bin/
TARGETS := $(addprefix $(OUTPUT_DIR)/,$(filter-out dd mknod,$(BIN))) $(OUTPUT_DIR)/ubase-dd $(OUTPUT_DIR)/ubase-mknod \
           $(OUTPUT_DIR)/reboot $(OUTPUT_DIR)/poweroff $(addprefix $(OUTPUT_DIR)/,useradd adduser usermod groupadd)

# ============================================================
# Флаги компиляции
# ============================================================
BASE_CFLAGS := $(filter-out -ffreestanding -std=gnu11,$(COMMON_CFLAGS))
CFLAGS += -D_GNU_SOURCE
CFLAGS := $(BASE_CFLAGS) \
	-D_XOPEN_SOURCE=700 \
	-D_DEFAULT_SOURCE \
	-D_GNU_SOURCE	\
	-Wall -Wextra

CPPFLAGS := $(COMMON_CPPFLAGS) -I. -MMD -MP

# ============================================================
# CRT и библиотеки musl
# ============================================================
CRTI := $(MUSL_LIB)/crti.o
CRT1 := $(MUSL_LIB)/crt1.o
CRTN := $(MUSL_LIB)/crtn.o

LIBS := --start-group \
	$(MUSL_LIB)/libc.a \
	$(MUSL_LIB)/libm.a \
	$(MUSL_LIB)/libpthread.a \
	$(MUSL_LIB)/libdl.a \
	$(MUSL_LIB)/librt.a \
	--end-group

# ============================================================
# Цели
# ============================================================
.PHONY: all clean libutil

all: $(TARGETS)

# Статическая библиотека libutil.a
$(LIBUTIL_A): $(LIBUTILOBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(OBJ_DIR)/libutil/%.o: libutil/%.c $(HDR) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Компиляция .c файлов утилит (без переименования main!)
$(OBJ_DIR)/%.o: %.c $(HDR) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Линковка каждого бинарника
$(OUTPUT_DIR)/%: $(OBJ_DIR)/%.o $(LIBUTIL_A) Makefile $(COMMON_DIR)/../Makefile
	@mkdir -p $(OUTPUT_DIR)
	$(LD) $(COMMON_LDFLAGS) \
	    $(CRTI) $(CRT1) \
	    $< \
	    $(LIBUTIL_A) \
	    $(LIBS) \
	    $(CRTN) \
	    $(APP_LINK_SCRIPT) \
	    -o $@

# Упаковщик базового образа принимает обычные файлы
$(OUTPUT_DIR)/reboot $(OUTPUT_DIR)/poweroff: $(OUTPUT_DIR)/halt
	cp -- $< $@

$(addprefix $(OUTPUT_DIR)/,useradd adduser usermod groupadd): $(OUTPUT_DIR)/passwd
	cp -- $< $@

# Keep both implementations without parallel builds overwriting sbase.
$(OUTPUT_DIR)/ubase-%: $(OBJ_DIR)/%.o $(LIBUTIL_A) Makefile $(COMMON_DIR)/../Makefile
	@mkdir -p $(OUTPUT_DIR)
	$(LD) $(COMMON_LDFLAGS) $(CRTI) $(CRT1) $< $(LIBUTIL_A) $(LIBS) $(CRTN) $(APP_LINK_SCRIPT) -o $@

# Order-only prerequisite
$(OBJ_DIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(TARGETS)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/libutil/*.d)