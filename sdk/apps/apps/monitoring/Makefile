TARGET := stats
OUTPUT_DIR ?= ../apps-bin
CC := $(abspath ../common/musl-cc)
TLS := ../common/mbedtls/upstream
LIBS := $(TLS)/library/libmbedtls.a $(TLS)/library/libmbedx509.a $(TLS)/library/libmbedcrypto.a
COMMON := ../common/settings
LINUX_CC ?= musl-gcc
LINUX_TLS := $(OUTPUT_DIR)/stats-linux-tls
LINUX_LIBS := $(LINUX_TLS)/library/libmbedtls.a $(LINUX_TLS)/library/libmbedx509.a \
	$(LINUX_TLS)/library/libmbedcrypto.a
.PHONY: all linux clean
all: $(OUTPUT_DIR)/$(TARGET)
linux: $(OUTPUT_DIR)/stats-linux
$(OUTPUT_DIR)/stats-linux: main.c performance.h $(COMMON)/settings.c $(COMMON)/settings.h \
	../common/tls/tls.c $(LINUX_LIBS)
	mkdir -p $(OUTPUT_DIR)
	$(LINUX_CC) -static -no-pie -DSTATS_LINUX -O2 -Wall -Wextra -Werror \
		-I$(LINUX_TLS)/include main.c $(COMMON)/settings.c ../common/tls/tls.c \
		$(LINUX_LIBS) -Wl,--build-id=none -o $@
$(LINUX_LIBS) &: Makefile $(TLS)/include/mbedtls/mbedtls_config.h
	mkdir -p $(LINUX_TLS)
	cp -a $(TLS)/. $(LINUX_TLS)/
	$(MAKE) -C $(LINUX_TLS)/library clean
	$(MAKE) -C $(LINUX_TLS)/library CC="$(LINUX_CC)" CFLAGS="-O2 -Wall -Wextra -Werror" static
$(OUTPUT_DIR)/$(TARGET): main.c performance.h $(COMMON)/settings.c $(COMMON)/settings.h ../common/tls/tls.c $(LIBS)
	mkdir -p $(OUTPUT_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -I$(TLS)/include main.c $(COMMON)/settings.c ../common/tls/tls.c $(LIBS) -Wl,--build-id=none -o $@
$(LIBS):
	$(MAKE) -C ../common/mbedtls
clean:
	rm -f $(OUTPUT_DIR)/$(TARGET)
