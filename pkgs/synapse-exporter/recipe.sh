# recipe.sh: Собрать экспортер метрик для SynapseOS
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: мониторинг
# Первое изменение: 23.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный

"$CC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$SDK/include" \
	"$port/main.c" -o "$payload/bin/synapse-exporter"
copy "$port/service" etc/services/synapse-exporter
