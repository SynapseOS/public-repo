# recipe.sh: собрать JSON-GLib с целевыми библиотеками GLib
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: обработка JSON для зависимостей Flatpak
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d /tmp/synapse-json-glib-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig"

for package in glib glib-gio libffi pcre2 zlib; do
	stage="$BUILD_ROOT/$package/payload/usr"
	[[ -d $stage ]]

	if [[ -d $stage/include ]]; then
		cp -a "$stage/include/." "$sysroot/usr/include/"
	fi

	if [[ -d $stage/lib ]]; then
		find "$stage/lib" -maxdepth 1 -type f -name '*.a' -exec cp -a {} "$sysroot/usr/lib/" \;
	fi

	if [[ -d $stage/lib/pkgconfig ]]; then
		cp -a "$stage/lib/pkgconfig/." "$sysroot/usr/lib/pkgconfig/"
	fi

	if [[ -d $stage/lib/glib-2.0 ]]; then
		cp -a "$stage/lib/glib-2.0" "$sysroot/usr/lib/"
	fi
done

cat >>"$sysroot/usr/lib/pkgconfig/glib-2.0.pc" <<'PC'
glib_mkenums=glib-mkenums
glib_genmarshal=glib-genmarshal
PC

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
c_args = ['-O2', '-D__synapse__']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

rm -rf -- "$work/build"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
	-Ddefault_library=static -Dtests=false -Dconformance=false \
	-Dinstalled_tests=false -Dintrospection=disabled -Ddocumentation=disabled \
	-Dnls=disabled -Dman=false

ninja -C "$work/build" -j"$JOBS" json-glib/libjson-glib-1.0.a
copy "$work/build/json-glib/libjson-glib-1.0.a" usr/lib/libjson-glib-1.0.a

for header in "$source/json-glib/"*.h "$work/build/json-glib/"*.h; do
	copy "$header" "usr/include/json-glib-1.0/json-glib/$(basename "$header")"
done

copy "$source/COPYING" usr/share/licenses/json-glib/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/json-glib-1.0.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: JSON-GLib
Description: JSON parser and generator for SynapseOS
Version: 1.10.8
Requires: gio-2.0
Libs: -L${libdir} -ljson-glib-1.0
Cflags: -I${includedir}/json-glib-1.0
PC
