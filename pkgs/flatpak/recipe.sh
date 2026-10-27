# recipe.sh: собрать CLI Flatpak для SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: сборка менеджера приложений без включения в образ по умолчанию
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 27.10.2026
# Статус доступа: общедоступный

prepare

fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"
sysroot=$(mktemp -d /tmp/synapse-flatpak-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig" "$sysroot/usr/bin"
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$sysroot"
ln -s "$sysroot/usr/include/x86_64-linux-gnu/asm" "$sysroot/usr/include/asm"

for package in glib glib-gio libffi pcre2 zlib libcap libarchive libcurl openssl-pic openssl \
	libxml2 libostree json-glib appstream gdk-pixbuf libseccomp gpgme \
	libgpg-error libassuan fuse3 liblzma libe2p libfyaml libxmlb libpng; do
	stage="$BUILD_ROOT/$package/payload/usr"
	[[ -d $stage ]]
	cp -a "$stage/." "$sysroot/usr/"
done

cat >"$sysroot/usr/lib/pkgconfig/zlib.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: zlib
Description: Compression library
Version: 1.3.2
Libs: -L${libdir} -lz
Cflags: -I${includedir}
PC

cat >"$sysroot/usr/lib/pkgconfig/libcrypto.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libcrypto
Description: OpenSSL cryptographic library
Version: 3.5.9
Libs: -L${libdir} -lcrypto
Cflags: -I${includedir}
PC

sed -i \
	-e 's/^Requires: glib-2.0 gio-2.0 gobject-2.0$/Requires: glib-2.0 gio-2.0 gobject-2.0 libcurl libxml-2.0 libfyaml xmlb/' \
	-e '/^Requires.private:/d' "$sysroot/usr/lib/pkgconfig/appstream.pc"
sed -i \
	-e 's/^Requires: gio-unix-2.0$/Requires: gio-unix-2.0 liblzma zlib libcurl gpgme gpg-error libcrypto/' \
	-e '/^Requires.private:/d' "$sysroot/usr/lib/pkgconfig/ostree-1.pc"

for tool in glib-mkenums glib-genmarshal glib-compile-resources gdbus-codegen; do
	ln -s "$(command -v "$tool")" "$sysroot/usr/bin/$tool"
done

printf '\nglib_mkenums=/usr/bin/glib-mkenums\nglib_genmarshal=/usr/bin/glib-genmarshal\n' \
	>>"$sysroot/usr/lib/pkgconfig/glib-2.0.pc"
printf '\nglib_compile_resources=/usr/bin/glib-compile-resources\ngdbus_codegen=/usr/bin/gdbus-codegen\n' \
	>>"$sysroot/usr/lib/pkgconfig/gio-2.0.pc"

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
c_args = ['-O2', '-fPIC', '-D__synapse__', '-isystem', '$(gcc -print-file-name=include)', '-I$sysroot/usr/include']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

meson setup "$work/build" "$source" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
	-Ddefault_library=static -Dtests=false -Dinstalled_tests=false \
	-Dsystem_helper=disabled -Dsystemd=disabled -Ddconf=disabled \
	-Dmalcontent=disabled -Dlibzstd=disabled -Dgir=disabled -Dgtkdoc=disabled \
	-Dman=disabled -Ddocbook_docs=disabled -Dxauth=disabled \
	-Dwayland_security_context=disabled -Dselinux_module=disabled -Dseccomp=disabled \
	-Dsystem_bubblewrap=/usr/bin/bwrap \
	-Dsystem_dbus_proxy=/bin/xdg-dbus-proxy \
	-Dsystem_fusermount=/usr/bin/fusermount3

ninja -C "$work/build" -j"$JOBS" app/flatpak
copy "$work/build/app/flatpak" bin/flatpak
strip --strip-unneeded "$payload/bin/flatpak"
copy "$source/COPYING" usr/share/licenses/flatpak/COPYING
