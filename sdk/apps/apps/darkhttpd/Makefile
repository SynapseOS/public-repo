# Compatibility target for the base image. The maintained port lives in pkgs/.
ROOT := $(abspath ../../../..)
PUBLIC_PACKAGES_ROOT ?= $(ROOT)
OUTPUT_DIR ?= $(abspath ../apps-bin)
.PHONY: all clean
all:
	$(MAKE) -C $(PUBLIC_PACKAGES_ROOT) darkhttpd
	@mkdir -p $(OUTPUT_DIR)
	cp $(PUBLIC_PACKAGES_ROOT)/pkgs/.work/darkhttpd/payload/bin/darkhttpd $(OUTPUT_DIR)/darkhttpd
clean:
	rm -f $(OUTPUT_DIR)/darkhttpd
