TARGET := qwen3
LIBC   := musl

SRCS_C   := $(wildcard *.c)
SRCS_S   := $(wildcard *.S)
SRCS_ASM := $(wildcard *.asm)

OBJ_DIR := obj
OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS_C))
OBJS += $(patsubst %.S,$(OBJ_DIR)/%.o,$(SRCS_S))
OBJS += $(patsubst %.asm,$(OBJ_DIR)/%.o,$(SRCS_ASM))

CFLAGS  := $(COMMON_CFLAGS)

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

all: $(OUTPUT_DIR)/$(TARGET)

$(OUTPUT_DIR)/$(TARGET): $(OBJS)
	@mkdir -p $(OUTPUT_DIR)
	$(LD) $(COMMON_LDFLAGS) \
	    $(CRTI) $(CRT1) \
	    $(OBJS) \
	    $(LIBS) \
	    $(CRTN) \
	    -T $(COMMON_DIR)/apps.ld \
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
	rm -rf $(OBJ_DIR) $(OUTPUT_DIR)/$(TARGET)

-include $(wildcard $(OBJ_DIR)/*.d)