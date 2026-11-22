# recipe.sh: Собрать медиаплеер VLC с видео и FFmpeg для SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектуры x86_64 и AArch64, ABI musl
# Подсистема: графические пакеты
# Первое изменение: 21.11.2026, последнее изменение: 22.11.2026
# Статус доступа: общедоступный

prepare
fetch "$(jq -er .mpg123_source "$spec")" "$(jq -er .mpg123_sha256 "$spec")" "$work/mpg123.tar.bz2"
fetch "$(jq -er .faad_source "$spec")" "$(jq -er .faad_sha256 "$spec")" "$work/faad.tar.gz"
fetch "$(jq -er .ffmpeg_source "$spec")" "$(jq -er .ffmpeg_sha256 "$spec")" "$work/ffmpeg.tar.xz"
fetch "$(jq -er .zlib_source "$spec")" "$(jq -er .zlib_sha256 "$spec")" "$work/zlib.tar.gz"
tar xf "$work/mpg123.tar.bz2" -C "$work"
tar xf "$work/faad.tar.gz" -C "$work"
tar xf "$work/ffmpeg.tar.xz" -C "$work"
tar xf "$work/zlib.tar.gz" -C "$work"
launcher_cc=$CC
export SYNAPSE_LINK_MODE=dynamic
probe=$BUILD_ROOT/qt6-base/stage
linkpaths="$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime:$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm:$probe/usr/lib:$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"
cxx=(bash "$ROOT/build/cxx-shared.sh")
cc_flags=(-idirafter "$(gcc -print-file-name=include)")
strip_tool=strip
if [[ $TARGET_ARCH == aarch64 ]]; then
    probe=${ARM64_DESKTOP_SYSROOT:?}
    gcc_object=$(find "$probe/usr/lib/gcc/aarch64-alpine-linux-musl" -name crtbeginS.o -print -quit)
    test -f "$gcc_object"
    cc_flags=(--target=aarch64-alpine-linux-musl "--sysroot=$probe" "--gcc-install-dir=$(dirname "$gcc_object")" -fuse-ld=lld)
    CC=${ARM64_CC:-clang}
    cxx=("${ARM64_CXX:-clang++}" "${cc_flags[@]}")
    export LD="$(command -v ld.lld)"
    linkpaths="$probe/usr/lib:$probe/lib"
    strip_tool=${ARM64_STRIP:-${ARM64_OBJCOPY:-llvm-objcopy}}
else
    fetch "$(jq -er .linux_headers_source "$spec")" "$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"
    mkdir -p "$work/headers"
    ar p "$work/linux-libc-dev.deb" data.tar.xz | tar xJ -C "$work/headers"
    cc_flags+=(-idirafter "$work/headers/usr/include/x86_64-linux-gnu" -idirafter "$work/headers/usr/include")
fi
export PKG_CONFIG_LIBDIR="$work/empty-pkgconfig" PKG_CONFIG_PATH=''
mkdir -p "$PKG_CONFIG_LIBDIR" "$work/mpg-build" "$work/vlc-build" "$work/ffmpeg-build" "$work/ffmpeg-install"
cflags="-O2 -fPIC ${cc_flags[*]}"
host="$TARGET_ARCH-linux-musl"
(
    cd "$work/mpg-build"
    CC="$CC" CFLAGS="$cflags" "$work/mpg123-1.33.7/configure" --build="$(gcc -dumpmachine)" \
        --host="$host" --with-cpu=generic --disable-shared --enable-static --with-pic --disable-components --enable-libmpg123
    make -j"$JOBS" src/libmpg123/libmpg123.la
)
cmake -S "$work/faad2-2.11.2" -B "$work/faad-build" -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_C_FLAGS="$cflags" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$work/faad-build" --target faad -j"$JOBS"
(
    cd "$work/ffmpeg-build"
    CC="$CC ${cc_flags[*]}" "$work/ffmpeg-4.4.6/configure" --prefix="$work/ffmpeg-install" \
        --arch="$TARGET_ARCH" --target_os=linux --cc="$CC ${cc_flags[*]}" --cxx="${cxx[*]}" \
        --extra-cflags="-I$BUILD_ROOT/zlib/payload/usr/include" \
        --extra-ldflags="-L$BUILD_ROOT/zlib/payload/usr/lib" \
        --disable-programs --disable-doc --disable-debug --disable-autodetect --enable-zlib \
        --enable-pic --disable-shared --enable-static
    make -j"$JOBS"
    make install
)
mkdir -p "$work/zlib-build"
(
    cd "$work/zlib-build"
    CC="$CC ${cc_flags[*]}" CFLAGS="-O2 -fPIC" "$work/zlib-1.3.2/configure" --prefix=/usr
    make -j"$JOBS" libz.so.1.3.2
)
copy "$work/zlib-build/libz.so.1.3.2" usr/lib/vlc/libz.so.1
export PKG_CONFIG_LIBDIR="$work/ffmpeg-install/lib/pkgconfig:$work/empty-pkgconfig"
ffmpeg_cflags="-I$work/ffmpeg-install/include"
avcodec_libs="-L$work/ffmpeg-install/lib -lavcodec -lavutil -lswresample -lm -lz -pthread"
avformat_libs="-L$work/ffmpeg-install/lib -lavformat -lavcodec -lavutil -lswresample -lm -lz -pthread"
swscale_libs="-L$work/ffmpeg-install/lib -lswscale -lavutil -lm -pthread"
(
    cd "$work/vlc-build"
    CC="$CC ${cc_flags[*]}" CXX="${cxx[*]}" CFLAGS=-O2 CXXFLAGS=-O2 \
        AVCODEC_CFLAGS="$ffmpeg_cflags" AVCODEC_LIBS="$avcodec_libs" \
        AVFORMAT_CFLAGS="$ffmpeg_cflags" AVFORMAT_LIBS="$avformat_libs" \
        AVUTIL_CFLAGS="$ffmpeg_cflags" AVUTIL_LIBS="-L$work/ffmpeg-install/lib -lavutil -lm -pthread" \
        SWSCALE_CFLAGS="$ffmpeg_cflags" SWSCALE_LIBS="$swscale_libs" \
        "$source/configure" --host="$host" --build="$(gcc -dumpmachine)" --prefix=/usr \
        --disable-nls --disable-lua --disable-sout --disable-dbus --enable-avcodec --enable-avformat \
        --enable-swscale --disable-postproc --disable-a52 --disable-qt --disable-skins2 --disable-xcb \
        --disable-wayland --disable-vdpau --disable-libgcrypt --disable-update-check --disable-dvdread \
        --disable-dvdnav --disable-bluray --disable-vcd --disable-chromecast --disable-vpx --disable-mad \
        --disable-libmpeg2 --disable-alsa --disable-pulse --disable-taglib --disable-live555
    for component in compat src lib bin; do make -C "$component" -j"$JOBS"; done
    copy "bin/.libs/vlc" usr/lib/vlc/vlc-bin
    modules=(filesystem es mp4 wav ps avi mpgv h26x rawvideo rawaud subsdec
             packetizer_mpegaudio packetizer_mpeg4audio packetizer_mpegvideo packetizer_mpeg4video
             packetizer_h264 packetizer_hevc packetizer_vc1 packetizer_flac packetizer_copy
             mpg123 faad avcodec avformat swscale araw adummy amem audio_format ugly_resampler
             trivial_channel_mixer simple_channel_mixer integer_mixer float_mixer cache_read skiptags
             console_logger vmem vdummy dummy hotkeys)
    targets=()
    for module in "${modules[@]}"; do targets+=("lib${module}_plugin.la"); done
    make -C modules -j"$JOBS" \
        MPG123_CFLAGS="-I$work/mpg123-1.33.7/src/include -I$work/mpg-build/src/include" \
        MPG123_LIBS="$work/mpg-build/src/libmpg123/.libs/libmpg123.a -lm" \
        CPPFLAGS_faad="-I$work/faad2-2.11.2/include -I$work/faad-build/include" \
        LIBS_faad="$work/faad-build/libfaad.a" "${targets[@]}"
    for module in "${modules[@]}"; do
        copy "modules/.libs/lib${module}_plugin.so" "usr/lib/vlc/plugins/audio/lib${module}_plugin.so"
    done
)
core=$work/vlc-build/src/.libs
api=$work/vlc-build/lib/.libs
copy "$core/libvlccore.so.9" usr/lib/vlc/libvlccore.so.9
copy "$api/libvlc.so.5" usr/lib/vlc/libvlc.so.5
mkdir -p "$payload/usr/libexec/qt6"
"${cxx[@]}" -O2 -std=c++17 -Wall -Wextra -Werror -fPIC -I"$SDK/include" -I"$source/include" \
    "$port/audio.cpp" -L"$api" -Wl,-rpath-link,"$linkpaths:$core" -lvlc -o "$payload/usr/libexec/vlc-audio"
"${cxx[@]}" -O2 -std=c++17 -Wall -Wextra -Werror -fPIC -I"$SDK/include" -I"$source/include" \
    -I"$probe/usr/include/qt6" -I"$probe/usr/include/qt6/QtWidgets" -I"$probe/usr/include/qt6/QtGui" \
    -I"$probe/usr/include/qt6/QtCore" \
    "$port/player.cpp" -L"$probe/usr/lib" -L"$api" -Wl,-rpath-link,"$linkpaths:$core" \
    -lQt6Widgets -lQt6Gui -lQt6Core -lQt6DBus -lvlc -o "$payload/usr/libexec/qt6/vlc-music"
SYNAPSE_LINK_MODE=static "$launcher_cc" -O2 '-DQT_APP_LIBRARY_PATH="/usr/lib/vlc:"' \
    '-DAPPLICATION_PATH="/usr/libexec/qt6/vlc-music"' "$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/vlc"
# libtool подставляет каталог сборки ядра VLC в RUNPATH своих модулей
find "$payload/usr/lib/vlc" -type f -exec patchelf --remove-rpath {} \;
find "$payload/usr/lib" "$payload/usr/libexec" -type f -exec "$strip_tool" --strip-unneeded {} \;
copy "$port/vlc.desktop" usr/share/applications/vlc.desktop
copy "$source/share/icons/48x48/vlc.png" usr/share/icons/hicolor/48x48/apps/vlc.png
copy "$source/COPYING" usr/share/licenses/vlc/COPYING
copy "$source/COPYING.LIB" usr/share/licenses/vlc/COPYING.LIB
copy "$work/faad2-2.11.2/COPYING" usr/share/licenses/vlc/FAAD2-COPYING
copy "$work/mpg123-1.33.7/COPYING" usr/share/licenses/vlc/mpg123-COPYING
copy "$work/ffmpeg-4.4.6/COPYING.LGPLv2.1" usr/share/licenses/vlc/FFmpeg-COPYING.LGPLv2.1
copy "$work/ffmpeg-4.4.6/COPYING.LGPLv3" usr/share/licenses/vlc/FFmpeg-COPYING.LGPLv3
