TARGET := lua
CC := $(abspath ../common/musl-cc)
SOURCES := lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c ldblib.c liolib.c lmathlib.c loslib.c ltablib.c lstrlib.c lutf8lib.c lcorolib.c linit.c loadlib.c lua.c
HEADERS := $(wildcard *.h)
.PHONY: all clean
all: $(OUTPUT_DIR)/$(TARGET) obj/lua-dynamic
obj/lua-dynamic: $(SOURCES) $(HEADERS) $(MUSL_LIB)/libc.so
	@mkdir -p obj
	SYNAPSE_LINK_MODE=dynamic $(CC) -O2 -DLUA_USE_POSIX -DLUA_USE_DLOPEN -Wl,--export-dynamic $(SOURCES) -lm -ldl -o $@
$(OUTPUT_DIR)/$(TARGET): launcher.c
	@mkdir -p $(OUTPUT_DIR)
	$(CC) -Os launcher.c -o $@
clean:
	rm -rf obj $(OUTPUT_DIR)/$(TARGET)
