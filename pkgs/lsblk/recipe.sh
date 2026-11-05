# recipe.sh: собрать lsblk для блочных устройств
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 30.10.2026, последнее изменение: 30.10.2026
# Статус доступа: общедоступный

"$CC" -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -static \
    "$port/lsblk.c" -o "$payload/bin/lsblk"
