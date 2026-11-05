# recipe.sh: собрать GNU tar под отдельным именем gtar
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
		--prefix=/usr --bindir=/bin --disable-nls
	make -j"$JOBS"
)

copy "$work/build/src/tar" bin/gtar
copy "$source/COPYING" usr/share/licenses/gnu-tar/COPYING
