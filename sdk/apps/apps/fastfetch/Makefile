# Makefile: сборка fastfetch для SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектуры x86_64 и AArch64, ABI musl SynapseOS
# Подсистема: пользовательские приложения
# Первое изменение: исходная дата не установлена, последнее изменение: 21.11.2026
# Статус доступа: общедоступный

OUTPUT_DIR ?= ../apps-bin
.PHONY: all clean
all: $(OUTPUT_DIR)/fastfetch
build/source/.patched: terminal-deadline.patch $(shell find upstream -type f)
	@mkdir -p build/source
	cp -a upstream/. build/source/
	patch -d build/source -p1 < terminal-deadline.patch
	@touch $@
build/cmake/Makefile: build/source/.patched toolchain.cmake Makefile ../common/musl-cc
	cmake -S build/source -B build/cmake -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DIS_MUSL=ON -DSET_TWEAK=OFF -DBUILD_FLASHFETCH=OFF -DBUILD_TESTS=OFF -DENABLE_APPLE_MEMSIZE_USABLE=OFF -DENABLE_ASAN=OFF -DENABLE_CHAFA=OFF -DENABLE_DBUS=OFF -DENABLE_DCONF=OFF -DENABLE_DDCUTIL=OFF -DENABLE_DRM=OFF -DENABLE_EET=OFF -DENABLE_EGL=OFF -DENABLE_ELF=OFF -DENABLE_EMBEDDED_AMDGPUIDS=OFF -DENABLE_EMBEDDED_PCIIDS=OFF -DENABLE_FREETYPE=OFF -DENABLE_GIO=OFF -DENABLE_GLX=OFF -DENABLE_IMAGEMAGICK6=OFF -DENABLE_IMAGEMAGICK7=OFF -DENABLE_IMAGE_LOGO=OFF -DENABLE_LIBZFS=OFF -DENABLE_LTO=OFF -DENABLE_LUA=OFF -DENABLE_OPENCL=OFF -DENABLE_PULSE=OFF -DENABLE_QUICKJS=OFF -DENABLE_RPM=OFF -DENABLE_SIXEL=OFF -DENABLE_SQLITE3=OFF -DENABLE_SYSTEM_YYJSON=OFF -DENABLE_THREADS=OFF -DENABLE_TRACER=OFF -DENABLE_VADRM=OFF -DENABLE_VAX11=OFF -DENABLE_VDPAU=OFF -DENABLE_VULKAN=OFF -DENABLE_WAYLAND=OFF -DENABLE_WCWIDTH=OFF -DENABLE_WIN81_COMPAT=OFF -DENABLE_WORDEXP=OFF -DENABLE_XCB_RANDR=OFF -DENABLE_XRANDR=OFF -DENABLE_ZLIB=OFF
$(OUTPUT_DIR)/fastfetch: build/cmake/Makefile build/source/.patched
	cmake --build build/cmake --target fastfetch -j4
	mkdir -p $(OUTPUT_DIR)
	cp build/cmake/fastfetch $@
clean:
	rm -rf build $(OUTPUT_DIR)/fastfetch
