TARGET := acme
OUTPUT_DIR ?= $(abspath ../apps-bin)
CC := $(abspath ../common/musl-cc)
TLS := ../common/mbedtls/upstream
LIBS := $(TLS)/library/libmbedtls.a $(TLS)/library/libmbedx509.a $(TLS)/library/libmbedcrypto.a
SOURCES := main.c http.c ../common/tls/tls.c ../qemu-ga/json.c
.PHONY: all clean
all: $(OUTPUT_DIR)/$(TARGET)
$(OUTPUT_DIR)/$(TARGET): $(SOURCES) acme.h $(LIBS)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) -O2 -Wall -Wextra -Wno-misleading-indentation -ffunction-sections -fdata-sections -I$(TLS)/include $(SOURCES) $(LIBS) -Wl,--gc-sections -o $@
$(LIBS):
	$(MAKE) -C ../common/mbedtls
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
