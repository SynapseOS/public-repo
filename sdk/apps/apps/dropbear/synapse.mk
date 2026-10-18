# Keep the generated upstream Makefile separate from this tracked entry point.
ROOT := $(abspath ..)
OUTPUT_DIR ?= $(ROOT)/apps-bin
BUILD := build-synapse
DRIVER := $(ROOT)/common/musl-cc

.PHONY: all clean
all: $(BUILD)/Makefile
	$(MAKE) -C $(BUILD) MULTI=1 PROGRAMS="dropbear dbclient dropbearkey dropbearconvert scp"
	mkdir -p $(OUTPUT_DIR)
	cp $(BUILD)/dropbearmulti $(OUTPUT_DIR)/dropbearmulti

$(BUILD)/Makefile: configure Makefile.in synapse-options.h synapse.mk $(DRIVER)
	mkdir -p $(BUILD)
	cp synapse-options.h $(BUILD)/localoptions.h
	cd $(BUILD) && CC="$(DRIVER)" CFLAGS="-Os" ../configure \
	    --host=x86_64-synapse-linux-musl --enable-static --disable-harden --disable-zlib \
	    --disable-openpty --disable-syslog --disable-lastlog --disable-utmp --disable-utmpx \
	    --disable-wtmp --disable-wtmpx --disable-loginfunc --disable-pututline \
	    --disable-pututxline --enable-bundled-libtom

clean:
	rm -rf $(BUILD) $(OUTPUT_DIR)/dropbearmulti
