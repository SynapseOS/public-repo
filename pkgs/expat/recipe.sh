# recipe.sh: собрать Expat для D-Bus
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека разбора XML без утилит хоста
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

(
	cd "$source"
	CC="$CC" CFLAGS='-O2 -Werror -fPIC' CPPFLAGS='-D__synapse__' \
		./configure --build="$(gcc -dumpmachine)" --host="$TARGET_ARCH-linux-musl" \
			--prefix=/usr --disable-shared --enable-static \
			--without-docbook --without-examples --without-tests \
			--without-xmlwf

	make -C lib -j"$JOBS" libexpat.la
)

copy "$source/lib/.libs/libexpat.a" usr/lib/libexpat.a
copy "$source/lib/expat.h" usr/include/expat.h
copy "$source/lib/expat_external.h" usr/include/expat_external.h
copy "$source/COPYING" usr/share/licenses/expat/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/expat.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: expat
Description: XML parser for SynapseOS
Version: 2.9.0
Libs: -L${libdir} -lexpat
Cflags: -I${includedir}
PC
