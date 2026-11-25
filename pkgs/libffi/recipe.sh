# recipe.sh: собрать статическую libffi с PIC для GObject
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64 / ARM64, ABI: synapse-x86_64-v1 / synapse-aarch64-v1
# Описание: интерфейс вызова функций для зависимостей Flatpak
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.11.2026
# Статус доступа: общедоступный

prepare
host=${TARGET_ARCH:-x86_64}-linux-musl
build_triple=$(gcc -dumpmachine)

(
	cd "$source"
	CC="$CC" \
	CFLAGS='-O2 -fPIC -Werror -Wno-deprecated-declarations' \
		./configure --build="$build_triple" --host="$host" \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static \
			--disable-docs --disable-raw-api

	make -j"$JOBS"
)

if [[ ${TARGET_ARCH:-x86_64} == aarch64 ]]; then
	build="$source/aarch64-unknown-linux-musl"
else
	build="$source/x86_64-pc-linux-musl"
fi

copy "$build/.libs/libffi.a" usr/lib/libffi.a
copy "$build/include/ffi.h" usr/include/ffi.h
copy "$build/include/ffitarget.h" usr/include/ffitarget.h
copy "$source/LICENSE" usr/share/licenses/libffi/LICENSE

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libffi.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libffi
Description: Foreign function interface library
Version: 3.8.0
Libs: -L${libdir} -lffi
Cflags: -I${includedir}
PC
