# recipe.sh: собрать статическую bzip2-библиотеку
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare
make -C "$source" -j"$JOBS" CC="$CC" CFLAGS='-O2 -D__synapse__' libbz2.a

copy "$source/libbz2.a" usr/lib/libbz2.a
copy "$source/bzlib.h" usr/include/bzlib.h
copy "$source/LICENSE" usr/share/licenses/libbz2/LICENSE
