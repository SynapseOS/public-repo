# recipe.sh: собрать GdkPixbuf для значков Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: встроенные загрузчики PNG и GIF
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-gdk-pixbuf-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig"

for package in glib glib-gio libffi pcre2 zlib libpng; do
	stage="$BUILD_ROOT/$package/payload/usr"
	[[ -d $stage ]]
	cp -a "$stage/." "$sysroot/usr/"
done

cat >>"$sysroot/usr/lib/pkgconfig/glib-2.0.pc" <<'PC'
glib_mkenums=glib-mkenums
glib_genmarshal=glib-genmarshal
PC

cat >"$sysroot/usr/lib/pkgconfig/zlib.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: zlib
Description: zlib
Version: 1.3.2
Libs: -L${libdir} -lz
Cflags: -I${includedir}
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
c_args = ['-O2', '-fPIC', '-D__synapse__', '-isystem', '$(gcc -print-file-name=include)']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

meson setup "$work/build" "$source" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
	-Ddefault_library=static -Dtests=false -Dinstalled_tests=false \
	-Dintrospection=disabled -Ddocumentation=false -Dman=false \
	-Dgio_sniffing=false -Dthumbnailer=disabled -Dglycin=disabled \
	-Dpng=enabled -Dgif=enabled -Djpeg=disabled -Dtiff=disabled \
	-Dbuiltin_loaders=png,gif

ninja -C "$work/build" -j"$JOBS" gdk-pixbuf/libgdk_pixbuf-2.0.a \
	gdk-pixbuf/libstaticpixbufloader-png.a gdk-pixbuf/libstaticpixbufloader-gif.a
copy "$work/build/gdk-pixbuf/libgdk_pixbuf-2.0.a" usr/lib/libgdk_pixbuf-2.0.a

for loader in png gif; do
	copy "$work/build/gdk-pixbuf/libstaticpixbufloader-$loader.a" \
		"usr/lib/libstaticpixbufloader-$loader.a"
done

for header in "$source/gdk-pixbuf/"*.h "$work/build/gdk-pixbuf/"*.h; do
	copy "$header" "usr/include/gdk-pixbuf-2.0/gdk-pixbuf/$(basename "$header")"
done

copy "$source/COPYING" usr/share/licenses/gdk-pixbuf/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/gdk-pixbuf-2.0.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: gdk-pixbuf-2.0
Description: Image loader for Flatpak icons
Version: 2.44.8
Requires: gio-2.0 libpng
Libs: -L${libdir} -lgdk_pixbuf-2.0 -lstaticpixbufloader-png -lstaticpixbufloader-gif
Cflags: -I${includedir}/gdk-pixbuf-2.0
PC
