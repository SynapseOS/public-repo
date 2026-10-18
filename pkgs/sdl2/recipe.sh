# recipe.sh: Собрать SDL2 с окнами Wayland и резервным прямым выводом
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

fetch 'https://www.libsdl.org/release/SDL2-2.30.11.tar.gz' \
	8b8d4aef2038533da814965220f88f77d60dfa0f32685f80ead65e501337da7f "$work/SDL2.tar.gz"
source="$work/SDL2-2.30.11"

if [[ ! -d $source ]]; then
	bsdtar -xf "$work/SDL2.tar.gz" -C "$work"
fi

if ! grep -q 'Synapse C99 backend' "$source/CMakeLists.txt"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/synapse-build.patch"
fi

cp "$ROOT/pkgs/mindustry-desktop/SDL_synapsevideo.c" "$source/src/video/offscreen/SDL_offscreenvideo.c"
cp "$port/synapse-wayland.inc" "$source/src/video/offscreen/"
cp "$BUILD_ROOT/weston/build/protocol/xdg-shell-client-protocol.h" "$source/src/video/offscreen/"
cp "$BUILD_ROOT/weston/build/protocol/xdg-shell-protocol.c" "$source/src/video/offscreen/synapse-xdg-shell-protocol.c"
flags="-D_GNU_SOURCE -DSDL_VIDEO_OPENGL=1 -isystem $(gcc -print-file-name=include)"
flags+=" -I$SDK/include -I$BUILD_ROOT/mesa/mesa-24.3.4/include"
flags+=" -I$SDK/apps/apps/nano-x/upstream/src/drivers -I$BUILD_ROOT/weston/sdk/usr/include"
cat >"$work/toolchain.cmake" <<CMAKE
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$CC")
set(CMAKE_CXX_COMPILER "$BUILD_ROOT/mesa/cxx")
set(CMAKE_C_FLAGS "$flags")
string(CONCAT CMAKE_SHARED_LINKER_FLAGS
    "-Wl,--no-as-needed -L$BUILD_ROOT/mesa/build/src/gallium/targets/osmesa "
    "-L$BUILD_ROOT/weston/client-payload/usr/lib "
    "-lOSMesa -l:libwayland-client.so.0 -l:libwayland-cursor.so.0 -l:libxkbcommon.so.0")
set(CMAKE_FIND_ROOT_PATH "$SDK/apps/musl")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CMAKE
export SYNAPSE_LINK_MODE=dynamic PKG_CONFIG_LIBDIR=/nonexistent
cmake -S "$source" -B "$work/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$work/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
	-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_DIRECTFB=OFF -DSDL_OPENGL=ON \
	-DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_OFFSCREEN=ON -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF \
	-DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=ON -DSDL_UDEV=OFF -DSDL_HIDAPI=OFF \
	-DSDL_DUMMYVIDEO=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF
cmake --build "$work/build" -j"$JOBS"
copy "$work/build/libSDL2-2.0.so.0" usr/lib/libSDL2-2.0.so.0
copy "$source/LICENSE.txt" usr/share/licenses/sdl2/LICENSE.txt
copy "$SDK/assets/fonts/dejavu-mono.psf" usr/share/sdl2/dejavu-mono.psf
copy "$SDK/assets/licenses/fonts/DejaVu-LICENSE.txt" usr/share/licenses/sdl2/DejaVu-LICENSE.txt
copy "$SDK/docs/user/opengl.md" usr/share/doc/sdl2/README.md
