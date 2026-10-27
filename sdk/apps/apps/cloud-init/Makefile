TARGET := cloud-init
OUTPUT_DIR ?= ../apps-bin
CC := ../common/musl-cc
CFLAGS := -O2 -Wall -Wextra -Werror -D_GNU_SOURCE
.PHONY: all clean
all: $(OUTPUT_DIR)/$(TARGET)
$(OUTPUT_DIR)/$(TARGET): cloud-init.c ../qemu-ga/json.c ../qemu-ga/json.h ../common/service_log.h ../common/musl-cc
	mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) cloud-init.c ../qemu-ga/json.c -o $@
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
