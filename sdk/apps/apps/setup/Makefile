TARGET := setup
OUTPUT_DIR ?= $(abspath ../apps-bin)
CC := $(abspath ../common/musl-cc)
TLS := ../common/mbedtls/upstream
CRYPTO := $(TLS)/library/libmbedcrypto.a
LIBS := $(TLS)/library/libmbedtls.a $(TLS)/library/libmbedx509.a $(CRYPTO)
SOURCES := progress.c plus.c ../common/tls/tls.c main.c common.c repository.c resolver.c archive.c transaction.c ed25519.c ../qemu-ga/json.c
.PHONY: all clean
all: $(OUTPUT_DIR)/$(TARGET)
$(OUTPUT_DIR)/$(TARGET): Makefile $(SOURCES) setup.h ../common/boot/limine-menu.h ../qemu-ga/json.h ../common/tls/tls.h $(LIBS)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) -O2 -Wall -Wextra -Wno-misleading-indentation -DJSON_TOKENS=131072 -ffunction-sections -fdata-sections -I$(TLS)/include $(SOURCES) $(LIBS) -Wl,--gc-sections -o $@
$(LIBS) &:
	$(MAKE) -C ../common/mbedtls
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
