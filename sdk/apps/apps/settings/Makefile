OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)
COMMON := ../common/settings
.PHONY: all clean
all: $(OUTPUT_DIR)/settings
$(OUTPUT_DIR)/settings: settings.c $(COMMON)/settings.c $(COMMON)/settings.h
	mkdir -p $(OUTPUT_DIR)
	$(CC) -O2 -Wall -Wextra settings.c $(COMMON)/settings.c -o $@
clean:
	rm -f $(OUTPUT_DIR)/settings
