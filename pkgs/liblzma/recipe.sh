# recipe.sh: собрать статическую liblzma из исходника XZ
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64 и AArch64, ABI: musl
# Описание: библиотека сжатия для OSTree
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.11.2026
# Статус доступа: общедоступный

prepare
CC=${LZMA_CC:-$CC}
host=${TARGET_ARCH:-x86_64}-linux-musl
build_triple=$(gcc -dumpmachine)

(
	cd "$source"
	CC="$CC" CFLAGS='-O2 -Werror -fPIC' CPPFLAGS='-D__synapse__' \
		./configure --build="$build_triple" --host="$host" \
			--prefix=/usr --disable-shared --enable-static --enable-threads=no \
			--disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo \
			--disable-scripts --disable-doc --disable-nls

	make -j"$JOBS" -C src/liblzma
)

copy "$source/src/liblzma/.libs/liblzma.a" usr/lib/liblzma.a
copy "$source/src/liblzma/api/lzma.h" usr/include/lzma.h

for header in "$source/src/liblzma/api/lzma/"*.h; do
	copy "$header" "usr/include/lzma/$(basename "$header")"
done

copy "$source/COPYING" usr/share/licenses/liblzma/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/liblzma.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: liblzma
Description: LZMA compression library
Version: 5.8.3
Libs: -L${libdir} -llzma
Cflags: -I${includedir}
PC
