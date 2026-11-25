# recipe.sh: собрать GObject и GIO отдельно от базового GLib
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64 / ARM64, ABI: synapse-x86_64-v1 / synapse-aarch64-v1
# Описание: расширения GLib для зависимостей Flatpak
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.11.2026
# Статус доступа: общедоступный

prepare
cpu=${TARGET_ARCH:-x86_64}
if [[ $cpu == aarch64 ]]; then
	compiler_headers=$("${ARM64_CC:-clang}" --print-resource-dir)/include
else
	compiler_headers=$(gcc -print-file-name=include)
fi

pcre="$BUILD_ROOT/pcre2/payload/usr"
ffi="$BUILD_ROOT/libffi/payload/usr"
zlib="$BUILD_ROOT/zlib/payload/usr"

for archive in "$pcre/lib/libpcre2-8.a" "$ffi/lib/libffi.a" "$zlib/lib/libz.a"; do
	[[ -f $archive ]]
done

cat >"$work/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = '/bin/false'

[host_machine]
system = 'linux'
cpu_family = '$cpu'
cpu = '$cpu'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-O2', '-D__synapse__', '-isystem', '$compiler_headers', '-I$pcre/include', '-I$ffi/include', '-I$zlib/include']
c_link_args = ['-static', '-L$pcre/lib', '-L$ffi/lib', '-L$zlib/lib']
CROSS

rm -rf -- "$work/build"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" --prefix=/usr --libdir=lib \
	--wrap-mode=nofallback -Ddefault_library=static -Dtests=false -Dnls=disabled \
	-Dintrospection=disabled -Dlibmount=disabled -Dselinux=disabled \
	-Dxattr=false -Dlibelf=disabled -Dsysprof=disabled -Dman-pages=disabled \
	-Ddocumentation=false -Ddtrace=disabled -Dsystemtap=disabled

ninja -C "$work/build" -j"$JOBS" gobject/libgobject-2.0.a \
	gio/libgio-2.0.a gmodule/libgmodule-2.0.a

copy "$work/build/gobject/libgobject-2.0.a" usr/lib/libgobject-2.0.a
copy "$work/build/gio/libgio-2.0.a" usr/lib/libgio-2.0.a
copy "$work/build/gmodule/libgmodule-2.0.a" usr/lib/libgmodule-2.0.a
copy "$source/glib/glib-object.h" usr/include/glib-2.0/glib-object.h
copy "$source/gmodule/gmodule.h" usr/include/glib-2.0/gmodule.h
copy "$work/build/gmodule/gmodule-visibility.h" usr/include/glib-2.0/gmodule/gmodule-visibility.h

for file in "$source/gobject/"*.h "$work/build/gobject/"*.h; do
	copy "$file" "usr/include/glib-2.0/gobject/$(basename "$file")"
done

for file in "$source/gio/"*.h "$work/build/gio/"*.h; do
	copy "$file" "usr/include/glib-2.0/gio/$(basename "$file")"
done

for file in "$source/gio/gdesktopappinfo.h" "$source/gio/gfiledescriptorbased.h" \
	"$source/gio/gunix"*.h; do
	copy "$file" "usr/include/gio-unix-2.0/gio/$(basename "$file")"
done

copy "$source/COPYING" usr/share/licenses/glib-gio/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/gobject-2.0.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: GObject
Description: GObject for SynapseOS
Version: 2.84.4
Libs: -L${libdir} -lgobject-2.0 -lglib-2.0 -lffi -lpcre2-8 -pthread -lm
Cflags: -I${includedir}/glib-2.0 -I${libdir}/glib-2.0/include
PC

cat >"$payload/usr/lib/pkgconfig/gio-unix-2.0.pc" <<'PC'
prefix=/usr
includedir=${prefix}/include
Name: GIO unix specific APIs
Description: Unix specific headers for GIO
Version: 2.84.4
Requires: gio-2.0
Cflags: -I${includedir}/gio-unix-2.0
PC

cat >"$payload/usr/lib/pkgconfig/gio-2.0.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: GIO
Description: GIO for SynapseOS
Version: 2.84.4
Libs: -L${libdir} -lgio-2.0 -lgmodule-2.0 -lgobject-2.0 -lglib-2.0 -lffi -lpcre2-8 -lz -pthread -lm
Cflags: -I${includedir}/glib-2.0 -I${libdir}/glib-2.0/include
PC

cat >"$payload/usr/lib/pkgconfig/gmodule-no-export-2.0.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: GModule
Description: GModule without exported process symbols
Version: 2.84.4
gmodule_supported=true
Requires: glib-2.0
Libs: -L${libdir} -lgmodule-2.0
Cflags: -I${includedir}/glib-2.0 -I${libdir}/glib-2.0/include
PC
