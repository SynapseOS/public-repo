TARGET := stats
OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)
TLS := ../common/mbedtls/upstream
LIBS := $(TLS)/library/libmbedtls.a $(TLS)/library/libmbedx509.a $(TLS)/library/libmbedcrypto.a
COMMON := ../common/settings
.PHONY: all clean
all: $(OUTPUT_DIR)/$(TARGET)
$(OUTPUT_DIR)/$(TARGET): main.c performance.h $(COMMON)/settings.c $(COMMON)/settings.h ../common/tls/tls.c $(LIBS)
	mkdir -p $(OUTPUT_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -I$(TLS)/include main.c $(COMMON)/settings.c ../common/tls/tls.c $(LIBS) -Wl,--build-id=none -o $@
$(LIBS):
	$(MAKE) -C ../common/mbedtls
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
