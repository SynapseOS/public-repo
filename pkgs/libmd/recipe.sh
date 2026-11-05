# recipe.sh: собрать статическую libmd для чтения архивов Debian
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS='-O2 -D__synapse__' LDFLAGS='-static' \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
		--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static
	make -j"$JOBS"
)

copy "$work/build/src/.libs/libmd.a" usr/lib/libmd.a
copy "$source/include/md5.h" usr/include/md5.h
copy "$source/COPYING" usr/share/licenses/libmd/COPYING
