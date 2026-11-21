ROOT := $(abspath ../../../..)
PUBLIC_PACKAGES_ROOT ?= $(ROOT)
BUILD_ROOT ?= $(PUBLIC_PACKAGES_ROOT)/pkgs/.work
OUTPUT_DIR ?= $(abspath ../apps-bin)
.PHONY: all clean
all:
	$(MAKE) -C $(PUBLIC_PACKAGES_ROOT) links
	mkdir -p $(OUTPUT_DIR)
	cp $(BUILD_ROOT)/links/payload/bin/links $(OUTPUT_DIR)/links
clean:
	rm -f $(OUTPUT_DIR)/links
