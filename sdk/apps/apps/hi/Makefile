OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)
COMMON := ../common/settings
.PHONY: all clean
all: $(OUTPUT_DIR)/hi
$(OUTPUT_DIR)/hi: hi.c installer.c installer.h $(COMMON)/settings.c $(COMMON)/settings.h
	mkdir -p $(OUTPUT_DIR)
	$(CC) -O2 -Wall -Wextra -I../../../include hi.c installer.c $(COMMON)/settings.c -o $@
clean:
	rm -f $(OUTPUT_DIR)/hi
