ROOT := $(abspath ../../..)
TOOLS := hwinfo thermal mod bootlog
.PHONY: all clean
all: $(addprefix $(OUTPUT_DIR)/,$(TOOLS))
$(OUTPUT_DIR)/%: %.c $(ROOT)/include/synapse/thermal.h $(ROOT)/include/synapse/gpu.h $(ROOT)/include/synapse/block.h
	@mkdir -p $(OUTPUT_DIR)
	$(COMMON_DIR)/musl-cc -O2 -Wall -Wextra -I$(ROOT)/include $< -o $@
clean:
	rm -f $(addprefix $(OUTPUT_DIR)/,$(TOOLS))
