# recipe.sh: Собрать утилиту управления, закрытый фильтр пакетов не используется
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

"$CC" -O2 -D_GNU_SOURCE -I"$SDK/include" "$SDK/apps/apps/firewall/firewall.c" \
	"$port/validate.c" -o "$payload/bin/firewall"
copy "$port/files/firewall.conf.example" etc/firewall.conf.example
