# recipe.sh: собрать OSTree с проверкой атрибутов и ограничением распаковки дельт
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека и команда для локальной проверки Flatpak
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare
export SYNAPSE_LINK_MODE=dynamic

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-ostree-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/lib/pkgconfig" "$sysroot/usr/include"

for package in glib glib-gio libffi pcre2 zlib liblzma libe2p libcurl openssl gpgme libassuan libgpg-error; do
	[[ -d $BUILD_ROOT/$package/payload/usr ]]
	cp -a "$BUILD_ROOT/$package/payload/usr/." "$sysroot/usr/"
done

for archive in libssl.a libcrypto.a; do
	copy_source="$BUILD_ROOT/openssl-pic/payload/usr/lib/pic/$archive"
	[[ -f $copy_source ]]
	cp "$copy_source" "$sysroot/usr/lib/$archive"
done

fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$sysroot"
ln -s "$sysroot/usr/include/x86_64-linux-gnu/asm" "$sysroot/usr/include/asm"

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

cat >"$sysroot/usr/lib/pkgconfig/libcrypto.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libcrypto
Description: OpenSSL crypto
Version: 3.5.9
Libs: -L${libdir} -lcrypto
Cflags: -I${includedir}
PC

host_tools="$work/host-tools"
mkdir -p "$host_tools"

if ! command -v bison >/dev/null || ! command -v m4 >/dev/null; then
	fetch "$(jq -er .host_bison_source "$spec")" \
		"$(jq -er .host_bison_sha256 "$spec")" "$work/host-bison.deb"
	fetch "$(jq -er .host_m4_source "$spec")" \
		"$(jq -er .host_m4_sha256 "$spec")" "$work/host-m4.deb"
	ar p "$work/host-bison.deb" data.tar.xz | tar -xJf - -C "$host_tools"
	ar p "$work/host-m4.deb" data.tar.xz | tar -xJf - -C "$host_tools"
	export PATH="$host_tools/usr/bin:$PATH"
	export M4="$host_tools/usr/bin/m4"
	export BISON_PKGDATADIR="$host_tools/usr/share/bison"
fi

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS='-O2 -fPIC -D__synapse__' \
		CPPFLAGS="-I$sysroot/usr/include -D__synapse__" \
		LDFLAGS="-L$sysroot/usr/lib" \
		ac_cv_have_decl_renameat2=yes ac_cv_have_decl_memfd_create=yes \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --disable-shared --enable-static --with-curl \
			--with-soup=no --with-soup3=no --with-gpgme=yes \
			--with-composefs=no --with-crypto=openssl --disable-http2 \
			--disable-rofiles-fuse --enable-introspection=no --disable-man

	make libglnx-config.h
	make -j"$JOBS" YACC=bison CPPFLAGS="-I$sysroot/usr/include -D__synapse__" \
		LDFLAGS="-L$sysroot/usr/lib" GLIB_MKENUMS=glib-mkenums \
		GLIB_GENMARSHAL=glib-genmarshal \
		GLIB_COMPILE_RESOURCES=glib-compile-resources libostree-1.la ostree

	make DESTDIR="$work/stage" install-libostreeincludeHEADERS
)

copy "$work/build/.libs/libostree-1.a" usr/lib/libostree-1.a
copy "$work/build/ostree" bin/ostree
copy "$work/build/src/libostree/ostree-1.pc" usr/lib/pkgconfig/ostree-1.pc

for header in "$work/stage/usr/include/ostree-1/"*.h; do
	copy "$header" "usr/include/ostree-1/$(basename "$header")"
done

copy "$source/COPYING" usr/share/licenses/libostree/COPYING
