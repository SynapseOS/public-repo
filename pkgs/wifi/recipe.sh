# recipe.sh: сборка утилиты беспроводной сети
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: пакет SynapseOS
# Описание: Упаковка /bin/wifi
# Подсистема: беспроводная сеть
# Первое изменение: 16.11.2026, последнее изменение: 16.11.2026
# Доступ: общедоступный

make -C "$SDK/apps/apps/wifi"
copy "$SDK/apps/apps/apps-bin/wifi" bin/wifi
copy "$port/files/hw-report" bin/hw-report
chmod 755 "$payload/bin/hw-report"
copy "$port/files/wifi" etc/services/wifi
