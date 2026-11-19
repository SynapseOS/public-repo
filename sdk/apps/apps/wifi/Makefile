OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)

.PHONY: all clean
all: $(OUTPUT_DIR)/wifi

$(OUTPUT_DIR)/wifi: wifi.c ../../../include/lumine/wifi.h
	mkdir -p $(OUTPUT_DIR)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I$(abspath ../../../include) wifi.c -o $@

clean:
	rm -f $(OUTPUT_DIR)/wifi
