# recipe.sh: собрать статическую библиотеку xxHash
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare
headers=$(gcc -print-file-name=include)
make -C "$source" -j"$JOBS" CC="$CC" \
	CFLAGS="-O2 -D__synapse__ -isystem $headers" libxxhash.a

copy "$source/libxxhash.a" usr/lib/libxxhash.a
copy "$source/xxhash.h" usr/include/xxhash.h
copy "$source/LICENSE" usr/share/licenses/libxxhash/LICENSE
