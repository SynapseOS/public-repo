# recipe.sh: Собрать клиент подписанных пакетов с открытой TLS-библиотекой
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

make -C "$SDK/apps/apps/setup"
copy "$SDK/apps/apps/apps-bin/setup" bin/setup
copy "$port/help.md" usr/share/help/setup.md
