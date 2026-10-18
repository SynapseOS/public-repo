TARGET := qemu-ga
OUTPUT_DIR ?= ../apps-bin
CC := ../common/musl-cc
CFLAGS := -O2 -Wall -Wextra -Werror -D_GNU_SOURCE

.PHONY: all clean
all: $(OUTPUT_DIR)/$(TARGET)
$(OUTPUT_DIR)/$(TARGET): qemu-ga.c json.c json.h ../common/service_log.h ../common/musl-cc ../common/apps.ld
	mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) qemu-ga.c json.c -o $@
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
