# recipe.sh: собрать libgpg-error для проверки подписей Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: таблицы ошибок и библиотека GnuPG
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS='-O2 -fPIC -Werror' ac_cv_func_snprintf=yes \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static \
			--disable-nls --disable-doc
	cp "$source/src/syscfg/lock-obj-pub.x86_64-unknown-linux-musl.h" \
		"$work/build/src/lock-obj-pub.native.h"
	make -j"$JOBS"
)

copy "$work/build/src/.libs/libgpg-error.a" usr/lib/libgpg-error.a
copy "$work/build/src/gpg-error.h" usr/include/gpg-error.h
copy "$work/build/src/gpgrt.h" usr/include/gpgrt.h
copy "$work/build/src/gpg-error-config" bin/gpg-error-config
copy "$source/COPYING" usr/share/licenses/libgpg-error/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/gpg-error.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: gpg-error
Description: GnuPG error definitions
Version: 1.61
Libs: -L${libdir} -lgpg-error
Cflags: -I${includedir}
PC
