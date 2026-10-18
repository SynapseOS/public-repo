# recipe.sh: Собрать библиотеку и оболочку SQLite с сохранением патча блокировок
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

local_spec="$work/source.json"
jq '.source=.url | .patches |= map("patches/"+.)' "$spec" >"$local_spec"
spec=$local_spec
prepare
flags=(-O2 -D__synapse__ -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_WAL -DSQLITE_MAX_MMAP_SIZE=0
	-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE
	-DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_DEFAULT_SYNCHRONOUS=2 -DSQLITE_USE_URI=1
	-ffunction-sections -fdata-sections)
mkdir -p "$payload/usr/lib"
"$CC" "${flags[@]}" -c "$source/sqlite3.c" -o "$work/sqlite3.o"
ar rcs "$payload/usr/lib/libsqlite3.a" "$work/sqlite3.o"
"$CC" "${flags[@]}" -I"$source" "$source/shell.c" "$work/sqlite3.o" -Wl,--gc-sections -lm -o "$payload/bin/sqlite3"

for header in sqlite3.h sqlite3ext.h; do
	copy "$source/$header" "usr/include/$header"
done

mkdir -p "$payload/usr/share/licenses/sqlite"

{
	printf 'SQLite is in the public domain\nhttps://sqlite.org/copyright.html\n'
	head -20 "$source/sqlite3.c"
} \
	>"$payload/usr/share/licenses/sqlite/LICENSE"
