# recipe.sh: собрать libarchive с целевой zlib
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека архивов для зависимостей Flatpak
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

zlib="$BUILD_ROOT/zlib/payload/usr"
[[ -f $zlib/lib/libz.a ]]

(
	cd "$source"
	CC="$CC" \
	CPPFLAGS="-D__synapse__ -I$zlib/include" \
	CFLAGS='-O2 -Werror' \
	LDFLAGS="-static -L$zlib/lib" \
	LIBS='-lz' \
		./configure --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --disable-shared --enable-static --disable-bsdtar \
			--disable-bsdcpio --disable-bsdcat --disable-bsdunzip \
			--without-bz2lib --without-lz4 --without-zstd --without-lzma \
			--without-xml2 --without-openssl --without-nettle --without-iconv

	make -j"$JOBS" libarchive.la
)

copy "$source/.libs/libarchive.a" usr/lib/libarchive.a
copy "$source/libarchive/archive.h" usr/include/archive.h
copy "$source/libarchive/archive_entry.h" usr/include/archive_entry.h
copy "$source/COPYING" usr/share/licenses/libarchive/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libarchive.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libarchive
Description: Archive reader and writer for SynapseOS
Version: 3.8.10
Libs: -L${libdir} -larchive -lz
Cflags: -I${includedir}
PC
