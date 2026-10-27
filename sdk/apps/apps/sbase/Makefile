# apps/sbase/Makefile
# Отдельные бинарники sbase для Синапс ОС (musl)

LIBC := musl

# ============================================================
# Список утилит для сборки (безопасный набор)
# ============================================================
# Build every utility whose source is present in this checkout.
BIN := $(sort $(basename $(notdir $(wildcard *.c))))

# ============================================================
# Исходники libutf и libutil
# ============================================================
LIBUTFSRC := \
	libutf/fgetrune.c      libutf/fputrune.c       \
	libutf/isalnumrune.c   libutf/isalpharune.c    \
	libutf/isblankrune.c   libutf/iscntrlrune.c    \
	libutf/isdigitrune.c   libutf/isgraphrune.c    \
	libutf/isprintrune.c   libutf/ispunctrune.c    \
	libutf/isspacerune.c   libutf/istitlerune.c    \
	libutf/isxdigitrune.c  libutf/lowerrune.c      \
	libutf/rune.c          libutf/runetype.c       \
	libutf/upperrune.c     libutf/utf.c            \
	libutf/utftorunestr.c

LIBUTILSRC := \
	libutil/concat.c       libutil/cp.c            \
	libutil/crypt.c        libutil/confirm.c       \
	libutil/ealloc.c       libutil/enmasse.c       \
	libutil/eprintf.c      libutil/eregcomp.c      \
	libutil/estrtod.c      libutil/fnck.c          \
	libutil/fshut.c        libutil/getlines.c      \
	libutil/human.c        libutil/linecmp.c       \
	libutil/md5.c          libutil/memmem.c        \
	libutil/mkdirp.c       libutil/mode.c          \
	libutil/parseoffset.c  libutil/putword.c       \
	libutil/reallocarray.c libutil/recurse.c       \
	libutil/rm.c           libutil/sha1.c          \
	libutil/sha224.c       libutil/sha256.c        \
	libutil/sha384.c       libutil/sha512.c        \
	libutil/sha512-224.c   libutil/sha512-256.c    \
	libutil/strcasestr.c   libutil/strlcat.c       \
	libutil/strlcpy.c      libutil/strsep.c        \
	libutil/strnsubst.c    libutil/strtonum.c      \
	libutil/unescape.c     libutil/writeall.c

HDR := arg.h compat.h crypt.h fs.h md5.h queue.h \
       sha1.h sha224.h sha256.h sha384.h sha512.h \
       sha512-224.h sha512-256.h text.h utf.h util.h

OBJ_DIR      := obj
LIBUTFOBJ    := $(patsubst libutf/%.c,$(OBJ_DIR)/libutf/%.o,$(LIBUTFSRC))
LIBUTILOBJ   := $(patsubst libutil/%.c,$(OBJ_DIR)/libutil/%.o,$(LIBUTILSRC))
LIBUTF_A     := $(OBJ_DIR)/libutf.a
LIBUTIL_A    := $(OBJ_DIR)/libutil.a

TARGETS := $(addprefix $(OUTPUT_DIR)/,$(BIN))

# ============================================================
# Флаги компиляции
# ============================================================
BASE_CFLAGS := $(filter-out -ffreestanding -std=gnu11,$(COMMON_CFLAGS))

CFLAGS := $(BASE_CFLAGS) \
	-std=c99 \
	-D_XOPEN_SOURCE=700 \
	-D_DEFAULT_SOURCE \
	-D_BSD_SOURCE \
	-D_NETBSD_SOURCE \
	-D_FILE_OFFSET_BITS=64 \
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
.PHONY: all clean libs

all: $(TARGETS)

$(OBJ_DIR)/sha256sum.o $(OBJ_DIR)/libutil/sha256.o: CFLAGS := $(filter-out -O0,$(CFLAGS)) -O2
$(OBJ_DIR)/sha256sum.o $(OBJ_DIR)/libutil/sha256.o: Makefile

libs: $(LIBUTF_A) $(LIBUTIL_A)

$(LIBUTF_A): $(LIBUTFOBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(LIBUTIL_A): $(LIBUTILOBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(OBJ_DIR)/libutf/%.o: libutf/%.c $(HDR) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/libutil/%.o: libutil/%.c $(HDR) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.c $(HDR) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Линковка каждого бинарника
$(OUTPUT_DIR)/%: $(OBJ_DIR)/%.o $(LIBUTF_A) $(LIBUTIL_A)
	@mkdir -p $(OUTPUT_DIR)
	$(LD) $(COMMON_LDFLAGS) \
	    $(CRTI) $(CRT1) \
	    $< \
	    $(LIBUTIL_A) \
	    $(LIBUTF_A) \
	    $(LIBS) \
	    $(CRTN) \
	    -T $(COMMON_DIR)/apps.ld \
	    -o $@

getconf.h: scripts/getconf.sh
	sh $< > $@

$(OBJ_DIR)/getconf.o: getconf.h

$(OBJ_DIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(TARGETS)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/libutf/*.d)
-include $(wildcard $(OBJ_DIR)/libutil/*.d)
