# Compatibility target; upstream sources and Synapse patches live in pkgs/doom.
ROOT := $(abspath ../../../..)
PUBLIC_PACKAGES_ROOT ?= $(ROOT)
BUILD_ROOT ?= $(PUBLIC_PACKAGES_ROOT)/pkgs/.work
OUTPUT_DIR ?= $(abspath ../apps-bin)
.PHONY: all clean
all:
	$(MAKE) -C $(PUBLIC_PACKAGES_ROOT) doom
	@mkdir -p $(OUTPUT_DIR)
	cp $(BUILD_ROOT)/doom/payload/bin/doom $(OUTPUT_DIR)/doom
clean:
	rm -f $(OUTPUT_DIR)/doom
