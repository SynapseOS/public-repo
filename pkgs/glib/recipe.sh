# recipe.sh: Собрать только ядро GLib со статической PCRE2
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
pcre="$BUILD_ROOT/pcre2/payload/usr"
cat >"$work/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = '/bin/false'
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-O2', '-D__synapse__', '-isystem', '$(gcc -print-file-name=include)', '-I$pcre/include']
c_link_args = ['-static', '-L$pcre/lib']
CROSS
rm -rf "$work/build"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" --prefix=/usr --libdir=lib \
	--wrap-mode=nofallback -Ddefault_library=static -Dtests=false -Dnls=disabled -Dintrospection=disabled \
	-Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dlibelf=disabled -Dsysprof=disabled \
	-Dman-pages=disabled -Ddocumentation=false -Ddtrace=disabled -Dsystemtap=disabled
ninja -C "$work/build" -j"$JOBS" glib/libglib-2.0.a
copy "$work/build/glib/libglib-2.0.a" usr/lib/libglib-2.0.a

for header in glib.h glib-unix.h; do
	copy "$source/glib/$header" "usr/include/glib-2.0/$header"
done

for file in "$source/glib/"*.h "$source/glib/deprecated/"*.h; do
	[[ $(basename "$file") == glib-object.h ]] &&
		continue
	copy "$file" "usr/include/glib-2.0/${file#"$source/"}"
done

for header in gversionmacros.h glib-visibility.h; do
	copy "$work/build/glib/$header" "usr/include/glib-2.0/glib/$header"
done

copy "$work/build/glib/glibconfig.h" usr/lib/glib-2.0/include/glibconfig.h
copy "$source/COPYING" usr/share/licenses/glib/COPYING

for file in "$source/LICENSES/"*; do
	copy "$file" "usr/share/licenses/glib/$(basename "$file")"
done

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/glib-2.0.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: GLib
Description: GLib core for SynapseOS
Version: 2.84.4
Libs: -L${libdir} -lglib-2.0 -lpcre2-8 -pthread -lm
Cflags: -I${includedir}/glib-2.0 -I${libdir}/glib-2.0/include
PC
