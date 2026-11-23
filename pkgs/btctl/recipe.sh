# recipe.sh: сборка утилиты управления Bluetooth
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: пакет SynapseOS
# Описание: Упаковка /bin/btctl и службы применения настроек
# Подсистема: Bluetooth
# Первое изменение: 23.11.2026, последнее изменение: 23.11.2026
# Доступ: общедоступный

make -C "$SDK/apps/apps/btctl"
copy "$SDK/apps/apps/apps-bin/btctl" bin/btctl
copy "$port/files/bt-auto" usr/libexec/bt-auto
mkdir -p "$payload/etc/services"
printf '%s\n' 'exec /bin/sh /usr/libexec/bt-auto' > "$payload/etc/services/bluetooth"
chmod 700 "$payload/etc/services/bluetooth"
chmod 755 "$payload/usr/libexec/bt-auto"
