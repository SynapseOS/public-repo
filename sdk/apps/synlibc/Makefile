CC ?= gcc
AR ?= ar
NASM ?= nasm
CFLAGS := -O2 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie -Wall -Wextra -MMD -MP
CPPFLAGS := -nostdinc -Iinclude -I../../include
LEGACY_C := synlibc synapi net vgpu string posix mem ini
ifneq ($(filter arm64 aarch64,$(ARCH)),)
ARM64_CC ?= clang
CC := $(ARM64_CC) --target=aarch64-linux-musl
AR := llvm-ar
TRANSPORT := obj/start-aarch64.o
else
CFLAGS += -mno-red-zone
TRANSPORT := obj/syscall.o obj/crt0.o
endif
LEGACY_OBJ := $(addprefix obj/,$(addsuffix .o,$(LEGACY_C))) $(TRANSPORT)
.PHONY: all clean
all: libsyn.a synlibc.a ../apps/synlibc.a
libsyn.a: obj/orbit.o obj/inference.o
	$(AR) rcs $@ $^
synlibc.a: $(LEGACY_OBJ) obj/orbit.o obj/inference.o
	$(AR) rcs $@ $^
../apps/synlibc.a: synlibc.a
	cp $< $@
obj/inference.o: override CFLAGS += -O3
obj/%.o: %.c
	mkdir -p obj
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
obj/%.o: %.S
	mkdir -p obj
	$(CC) -c $< -o $@
obj/%.o: %.asm
	mkdir -p obj
	$(NASM) -f elf64 $< -o $@
clean:
	rm -rf obj libsyn.a synlibc.a ../apps/synlibc.a
-include $(wildcard obj/*.d)
