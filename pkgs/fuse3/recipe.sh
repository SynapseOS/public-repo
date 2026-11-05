# recipe.sh: собрать пользовательскую библиотеку FUSE 3
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: зависимость сборки Flatpak, требует драйвер /dev/fuse для монтирования
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-fuse-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$sysroot"
ln -s "$sysroot/usr/include/x86_64-linux-gnu/asm" "$sysroot/usr/include/asm"

cat >"$work/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-O2', '-fPIC', '-D__synapse__', '-I$sysroot/usr/include', '-isystem', '$(gcc -print-file-name=include)']
CROSS

meson setup "$work/build" "$source" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
	-Ddefault_library=static -Dutils=false -Dexamples=false -Dtests=false \
	-Ddisable-libc-symbol-version=true -Denable-io-uring=false -Ddisable-mtab=true

ninja -C "$work/build" -j"$JOBS" lib/libfuse3.a
copy "$work/build/lib/libfuse3.a" usr/lib/libfuse3.a

for header in "$source/include/"*.h "$work/build/libfuse_config.h"; do
	copy "$header" "usr/include/fuse3/$(basename "$header")"
done

copy "$source/LICENSE" usr/share/licenses/fuse3/LICENSE

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/fuse3.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: fuse3
Description: FUSE 3 user library
Version: 3.18.3
Libs: -L${libdir} -lfuse3 -lpthread
Cflags: -I${includedir}/fuse3
PC
