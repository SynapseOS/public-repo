# recipe.sh: собрать nPth для GnuPG
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 26.10.2026
# Статус доступа: общедоступный

prepare

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS='-O2 -fPIC' \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static
	make -j"$JOBS"
)

copy "$work/build/src/.libs/libnpth.a" usr/lib/libnpth.a
copy "$work/build/src/npth.h" usr/include/npth.h
copy "$work/build/npth-config" bin/npth-config
copy "$source/COPYING.LIB" usr/share/licenses/npth/COPYING.LIB
