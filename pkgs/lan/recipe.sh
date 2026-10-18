# recipe.sh: Собрать службу обнаружения в локальной сети
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

make -C "$SDK/apps/apps/lan"
copy "$SDK/apps/apps/apps-bin/lan" bin/lan
copy "$port/files/lan-discovery" etc/services/lan-discovery
