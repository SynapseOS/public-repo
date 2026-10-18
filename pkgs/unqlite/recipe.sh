# recipe.sh: Собрать статическую библиотеку и интерфейс UnQLite
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
mkdir -p "$payload/usr/lib"
flags=(-O2 -D__synapse__ -D_DEFAULT_SOURCE -DUSE_PREAD -DUSE_PWRITE -ffunction-sections -fdata-sections -I"$source")
"$CC" "${flags[@]}" -c "$source/unqlite.c" -o "$work/unqlite.o"
ar rcs "$payload/usr/lib/libunqlite.a" "$work/unqlite.o"
"$CC" "${flags[@]}" "$port/unqlite-cli.c" "$payload/usr/lib/libunqlite.a" -Wl,--gc-sections -o "$payload/bin/unqlite"
copy "$source/unqlite.h" usr/include/unqlite.h
copy "$source/LICENSE" usr/share/licenses/unqlite/LICENSE
