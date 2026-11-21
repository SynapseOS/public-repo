#!/usr/bin/env bash
# Сборка PulseAudio и воспроизводящего OSS-модуля для рабочего стола AArch64.
# Автор: Елчинян Арен Артурович, 2026. Подсистема: звук SynapseOS.
set -eu -o pipefail
if (( $# != 4 )); then
    echo "Usage: $0 source-cache musl-sysroot build-directory install-directory" >&2
    exit 2
fi
mkdir -p "$1"
cache=$(realpath "$1")
sysroot=$(realpath "$2")
mkdir -p "$3" "$4"
work=$(realpath "$3")
stage=$(realpath "$4")
recipe=$(dirname "$(realpath "$0")")/port.json
jobs=${JOBS:-4}
test -f "$sysroot/lib/ld-musl-aarch64.so.1"
test "$(df --output=avail -B1 "$work" | tail -1)" -gt 22000000000

# Проверять архивы по тем же хешам, что и пакет AMD64.
for pair in 'source source.tar' 'sndfile_source sndfile.tar.xz' 'libtool_source libtool.tar.xz'; do
    read -r key filename <<< "$pair"
    hash=${key%source}sha256
    if [[ ! -f $cache/$filename ]]; then
        curl --fail --location --retry 3 -o "$work/$filename.part" "$(jq -er ".$key" "$recipe")"
        echo "$(jq -er ".$hash" "$recipe")  $work/$filename.part" | sha256sum -c -
        mv "$work/$filename.part" "$cache/$filename"
    fi
    echo "$(jq -er ".$hash" "$recipe")  $cache/$filename" | sha256sum -c -
    tar -xf "$cache/$filename" -C "$work"
done
deps="$work/deps"
mkdir -p "$deps/include"
# musl предоставляет gettext в libc. Не подмешивать макросы GNU libintl из SDK Alpine.
musl_include=$(dirname "$(realpath "$0")")/../../sdk/apps/musl/include
install -m644 "$musl_include/libintl.h" "$deps/include/libintl.h"
gccdir=$(dirname "$(find "$sysroot/usr/lib/gcc" -name crtbeginS.o -print -quit)")
test -f "$gccdir/crtbeginS.o"
export CC="$work/aarch64-cc"
cat > "$CC" <<EOF
#!/bin/sh
exec clang --target=aarch64-alpine-linux-musl --sysroot="$sysroot" --gcc-install-dir="$gccdir" -fuse-ld=lld -Wl,--undefined-version -Wno-unused-command-line-argument "\$@"
EOF
chmod +x "$CC"
cmake -S "$work/libsndfile-1.2.2" -B "$work/sndfile-build" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_COMPILER_TARGET=aarch64-alpine-linux-musl \
    -DCMAKE_SYSROOT="$sysroot" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DENABLE_EXTERNAL_LIBS=OFF \
    -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_SSE2=OFF -DENABLE_BOW_DOCS=OFF
cmake --build "$work/sndfile-build" -j"$jobs"
cmake --install "$work/sndfile-build"
mkdir -p "$work/ltdl-build"
(cd "$work/ltdl-build"
 if [[ ! -f "$deps/lib/libltdl.a" ]]; then
    CFLAGS='-O2 -fPIC' "$work/libtool-2.5.4/libltdl/configure" \
        --host=aarch64-linux-musl --prefix="$deps" --enable-ltdl-install --enable-static --disable-shared
 fi
 make -j"$jobs"
 make install)
cat > "$work/cross.ini" <<EOF
[binaries]
c = '$CC'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$deps/lib/pkgconfig'
[built-in options]
c_args = ['-I$deps/include']
c_link_args = ['-L$deps/lib', '-Wl,-rpath,/usr/lib/pulseaudio:/usr/lib']
EOF
meson setup "$work/pulse-build" "$work/pulseaudio-17.0" --cross-file "$work/cross.ini" \
    --prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
    -Dauto_features=disabled -Ddaemon=true -Dclient=true -Doss-output=enabled \
    -Ddatabase=simple -Ddoxygen=false -Dman=false -Dtests=false -Ddefault_library=shared
sed -i '/^#define HAVE_PTHREAD_PRIO_INHERIT 1$/d' "$work/pulse-build/config.h"
ninja -C "$work/pulse-build" -j"$jobs"
DESTDIR="$stage" meson install -C "$work/pulse-build" --no-rebuild
install -Dm644 "$work/pulseaudio-17.0/LICENSE" "$stage/usr/share/licenses/pulseaudio-oss/LICENSE"
install -Dm644 "$work/libsndfile-1.2.2/COPYING" "$stage/usr/share/licenses/pulseaudio-oss/libsndfile-COPYING"
install -Dm644 "$work/libtool-2.5.4/COPYING" "$stage/usr/share/licenses/pulseaudio-oss/libtool-COPYING"
