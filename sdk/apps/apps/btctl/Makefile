OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)

.PHONY: all clean
all: $(OUTPUT_DIR)/btctl

$(OUTPUT_DIR)/btctl: btctl.c ../../../include/lumine/bthci.h
	mkdir -p $(OUTPUT_DIR)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I$(abspath ../../../include) btctl.c -o $@

clean:
	rm -f $(OUTPUT_DIR)/btctl
