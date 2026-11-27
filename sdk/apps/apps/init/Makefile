TARGET := init
LIBC   := musl

# ramroot.c собирается отдельным бинарником /bin/ramroot, а не частью init
SRCS_C   := $(filter-out ramroot.c,$(wildcard *.c))
SRCS_S   := $(wildcard *.S)
SRCS_ASM := $(wildcard *.asm)

OBJ_DIR := obj
OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS_C))
OBJS += $(patsubst %.S,$(OBJ_DIR)/%.o,$(SRCS_S))
OBJS += $(patsubst %.asm,$(OBJ_DIR)/%.o,$(SRCS_ASM))
RAMROOT_OBJ := $(OBJ_DIR)/ramroot.o

CFLAGS  := $(COMMON_CFLAGS) $(COMMON_CPPFLAGS)
CPPFLAGS := -D_XOPEN_SOURCE=700 -MMD -MP

# ramroot использует zlib, payload собирается пакетом pkgs/zlib
ifndef BUILD_ROOT
BUILD_ROOT := $(abspath $(CURDIR)/../../../../pkgs/.work$(if $(filter aarch64,$(MUSL_ARCH)),-aarch64,))
endif
ZLIB_PREFIX := $(BUILD_ROOT)/zlib/payload/usr
CPPFLAGS += -I$(ZLIB_PREFIX)/include
ZLIB_LIB := $(ZLIB_PREFIX)/lib/libz.a

ifeq ($(LIBC),musl)
    CRTI   := $(MUSL_LIB)/crti.o
    CRT1   := $(MUSL_LIB)/crt1.o
    CRTN   := $(MUSL_LIB)/crtn.o
    LIBS   := --start-group \
              $(MUSL_LIB)/libc.a \
              $(MUSL_LIB)/libm.a \
              $(MUSL_LIB)/libpthread.a \
              $(MUSL_LIB)/libdl.a \
              $(MUSL_LIB)/librt.a \
              --end-group
else
    CFLAGS := $(COMMON_CFLAGS) -isystem $(SYNLIBC_INC)
    SYNLIBC_A := ../synlibc.a
    CRTI :=
    CRT1 := 
    CRTN :=
    LIBS := $(SYNLIBC_A)
endif

.PHONY: all clean

all: $(OUTPUT_DIR)/$(TARGET) $(OUTPUT_DIR)/ramroot

$(OUTPUT_DIR)/$(TARGET): $(OBJS) Makefile $(COMMON_DIR)/../Makefile
	@mkdir -p $(OUTPUT_DIR)
	$(LD) $(COMMON_LDFLAGS) \
	    $(CRTI) $(CRT1) \
	    $(OBJS) \
	    $(LIBS) \
	    $(CRTN) \
	    $(APP_LINK_SCRIPT) \
	    -o $@

$(OUTPUT_DIR)/ramroot: $(RAMROOT_OBJ) Makefile $(COMMON_DIR)/../Makefile
	@mkdir -p $(OUTPUT_DIR)
	$(LD) $(COMMON_LDFLAGS) \
	    $(CRTI) $(CRT1) \
	    $(RAMROOT_OBJ) \
	    $(ZLIB_LIB) \
	    $(LIBS) \
	    $(CRTN) \
	    $(APP_LINK_SCRIPT) \
	    -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.asm
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(OUTPUT_DIR)/$(TARGET) $(OUTPUT_DIR)/ramroot

-include $(wildcard $(OBJ_DIR)/*.d)