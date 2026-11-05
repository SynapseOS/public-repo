# recipe.sh: собрать lspci для PCI sysfs
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 29.10.2026, последнее изменение: 29.10.2026
# Статус доступа: общедоступный

"$CC" -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -static \
    "$port/lspci.c" -o "$payload/bin/lspci"
