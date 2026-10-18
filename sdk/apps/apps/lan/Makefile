TARGET := lan
OUTPUT_DIR ?= ../apps-bin
CC := ../common/musl-cc
CFLAGS := -O2 -Wall -Wextra -Werror -D_GNU_SOURCE
all: $(OUTPUT_DIR)/$(TARGET)
$(OUTPUT_DIR)/$(TARGET): lan.c protocol.h ../common/musl-cc ../common/apps.ld
	mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) lan.c -o $@
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
