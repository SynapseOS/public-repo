# recipe.sh: собрать libxml2 для зависимостей Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека XML без необязательных расширений
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

(
	cd "$source"
	CC="$CC" CFLAGS='-O2 -Werror' CPPFLAGS='-D__synapse__' \
		./configure --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --disable-shared --enable-static --without-python \
			--without-lzma --without-zlib --without-iconv

	make -j"$JOBS" libxml2.la
)

copy "$source/.libs/libxml2.a" usr/lib/libxml2.a

for header in "$source/include/libxml/"*.h; do
	copy "$header" "usr/include/libxml2/libxml/$(basename "$header")"
done

copy "$source/include/libxml/xmlversion.h" usr/include/libxml2/libxml/xmlversion.h
copy "$source/Copyright" usr/share/licenses/libxml2/Copyright

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libxml-2.0.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libXML
Description: XML library for SynapseOS
Version: 2.14.6
Libs: -L${libdir} -lxml2 -lm
Cflags: -I${includedir}/libxml2
PC
