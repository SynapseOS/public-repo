TARGET := service
OUTPUT_DIR ?= ../apps-bin
CC := ../common/musl-cc
CFLAGS := -O2 -Wall -Wextra -Werror -D_GNU_SOURCE

.PHONY: all clean
all: $(OUTPUT_DIR)/$(TARGET)
$(OUTPUT_DIR)/$(TARGET): service.c ../common/service_log.h ../common/musl-cc ../common/apps.ld
	mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) service.c -o $@
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
