# recipe.sh: собрать libpng для графических метаданных Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: проверка значков PNG с исправлением уязвимости версии 1.6.59
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

zlib="$BUILD_ROOT/zlib/payload/usr"
[[ -f $zlib/lib/libz.a ]]

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS="-O2 -fPIC -I$zlib/include" \
		CPPFLAGS="-I$zlib/include" \
		LDFLAGS="-L$zlib/lib" \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static
	make -j"$JOBS" libpng16.la
)

copy "$work/build/.libs/libpng16.a" usr/lib/libpng16.a
copy "$source/png.h" usr/include/png.h
copy "$source/pngconf.h" usr/include/pngconf.h
copy "$work/build/pnglibconf.h" usr/include/pnglibconf.h
copy "$source/LICENSE" usr/share/licenses/libpng/LICENSE

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libpng.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libpng
Description: PNG reference library
Version: 1.6.59
Requires: zlib
Libs: -L${libdir} -lpng16
Cflags: -I${includedir}
PC
