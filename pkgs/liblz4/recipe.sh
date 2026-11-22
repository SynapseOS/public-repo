# recipe.sh: собрать статическую LZ4-библиотеку
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare
env -u TARGET_ARCH make -C "$source/lib" -j"$JOBS" CC="$CC" CFLAGS='-O2 -D__synapse__' liblz4.a

copy "$source/lib/liblz4.a" usr/lib/liblz4.a

for header in lz4.h lz4frame.h lz4frame_static.h lz4hc.h; do
	copy "$source/lib/$header" "usr/include/$header"
done

copy "$source/lib/LICENSE" usr/share/licenses/liblz4/LICENSE
