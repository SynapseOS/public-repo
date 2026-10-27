# recipe.sh: Упаковать проверенный статический выпуск Alertmanager
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: мониторинг
# Первое изменение: 23.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный

prepare

copy "$source/alertmanager" bin/alertmanager
copy "$source/amtool" bin/amtool
chmod 755 "$payload/bin/alertmanager" "$payload/bin/amtool"
"$CC" -O2 -std=c11 -Wall -Wextra -Werror "$port/start.c" -o "$payload/bin/alertmanager-start"
copy "$port/alertmanager.yml" etc/alertmanager/alertmanager.yml
copy "$port/service" etc/services/alertmanager
copy "$source/LICENSE" usr/share/licenses/alertmanager/LICENSE
