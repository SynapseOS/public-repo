ROOT := $(abspath ../../../..)
PUBLIC_PACKAGES_ROOT ?= $(ROOT)
.PHONY: all clean
all:
	$(MAKE) -C $(PUBLIC_PACKAGES_ROOT) netcat
	mkdir -p $(OUTPUT_DIR)
	cp $(PUBLIC_PACKAGES_ROOT)/pkgs/.work/netcat/payload/bin/nc $(OUTPUT_DIR)/nc
clean:
	rm -f $(OUTPUT_DIR)/nc
