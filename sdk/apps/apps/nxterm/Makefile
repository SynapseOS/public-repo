ROOT := $(abspath ../../../..)
PUBLIC_PACKAGES_ROOT ?= $(ROOT)
OUTPUT_DIR ?= ../apps-bin
.PHONY: all clean
all:
	$(MAKE) -C $(PUBLIC_PACKAGES_ROOT) nxterm
	mkdir -p $(OUTPUT_DIR)
	cp $(PUBLIC_PACKAGES_ROOT)/pkgs/.work/nxterm/payload/bin/nxterm $(OUTPUT_DIR)/nxterm
clean:
	rm -f $(OUTPUT_DIR)/nxterm
