ROOT := $(abspath ../../../..)
PUBLIC_PACKAGES_ROOT ?= $(ROOT)
BUILD_ROOT ?= $(PUBLIC_PACKAGES_ROOT)/pkgs/.work
.PHONY: all clean
all:
	$(MAKE) -C $(PUBLIC_PACKAGES_ROOT) netcat
	mkdir -p $(OUTPUT_DIR)
	cp $(BUILD_ROOT)/netcat/payload/bin/nc $(OUTPUT_DIR)/nc
clean:
	rm -f $(OUTPUT_DIR)/nc
