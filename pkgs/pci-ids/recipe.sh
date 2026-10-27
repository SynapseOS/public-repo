# recipe.sh: Установить проверенную базу имён устройств PCI
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

checksum=$(jq -er '.sha256' "$port/port.json")
printf '%s  %s\n' "$checksum" "$port/pci.ids" | sha256sum --check --status

copy "$port/pci.ids" "usr/share/hwdata/pci.ids"
