CC := $(abspath ../musl-cc)
.PHONY: all clean
all: upstream/library/libmbedtls.a upstream/library/libmbedx509.a upstream/library/libmbedcrypto.a
upstream/library/libmbedtls.a upstream/library/libmbedx509.a upstream/library/libmbedcrypto.a &: Makefile
	mkdir -p upstream/tests
	$(MAKE) -C upstream/library CC="$(CC)" CFLAGS="-O2 -ffunction-sections -fdata-sections" static
clean:
	$(MAKE) -C upstream/library clean
