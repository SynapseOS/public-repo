# Makefile: сборка nano-x для SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI synapse-x86_64-v1
# Подсистема: пользовательские приложения
# Первое изменение: исходная дата не установлена, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

OUTPUT_DIR ?= ../apps-bin
NANO_CC := $(abspath ../common/musl-cc)
SRC := $(CURDIR)/upstream/src
.PHONY: all clean
all: $(OUTPUT_DIR)/nano-x
$(SRC)/lib/libnano-X.a: $(shell find upstream/src/include upstream/src/drivers upstream/src/engine upstream/src/nanox upstream/src/fonts -type f \( -name '*.c' -o -name '*.h' -o -name '*.rules' \)) $(SRC)/config
	$(MAKE) -C $(SRC)/nanox CC='$(NANO_CC)' AR=ar HOSTCC=gcc
$(OUTPUT_DIR)/nano-x: demo.c $(SRC)/lib/libnano-X.a
	mkdir -p $(OUTPUT_DIR)
	$(NANO_CC) -O2 -I$(SRC)/include demo.c $(SRC)/lib/libnano-X.a -lm -o $@
clean:
	$(MAKE) -C $(SRC)/nanox clean
	rm -f $(OUTPUT_DIR)/nano-x
