#!/bin/bash
# recipe.sh: Собрать GLFW для окон Wayland и Minecraft
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 23.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный

prepare

wayland_sdk="$BUILD_ROOT/weston/sdk"
export SYNAPSE_LINK_MODE=dynamic
export PKG_CONFIG_SYSROOT_DIR="$wayland_sdk"
export PKG_CONFIG_LIBDIR="$wayland_sdk/usr/lib/pkgconfig:$wayland_sdk/usr/share/pkgconfig"

cat >"$work/toolchain.cmake" <<CMAKE
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$CC")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS "-isystem $(gcc -print-file-name=include) -I$SDK/include -I$wayland_sdk/usr/include -I$BUILD_ROOT/mesa/linux-headers/usr/include")
set(CMAKE_FIND_ROOT_PATH "$SDK/apps/musl" "$wayland_sdk")
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CMAKE

cmake -S "$source" -B "$work/build" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$work/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-DGLFW_BUILD_WAYLAND=ON \
	-DGLFW_BUILD_X11=OFF \
	-DGLFW_BUILD_DOCS=OFF \
	-DGLFW_BUILD_EXAMPLES=OFF \
	-DGLFW_BUILD_TESTS=OFF \
	-DGLFW_INSTALL=OFF

cmake --build "$work/build" -j"$JOBS"

copy "$work/build/src/libglfw.so.3.4" usr/lib/libglfw.so
copy "$work/build/src/libglfw.so.3.4" usr/lib/libglfw.so.3
copy "$source/include/GLFW/glfw3.h" usr/include/GLFW/glfw3.h
copy "$source/include/GLFW/glfw3native.h" usr/include/GLFW/glfw3native.h
copy "$source/LICENSE.md" usr/share/licenses/glfw/LICENSE.md
