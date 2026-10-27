# recipe.sh: Упаковка Prometheus со встроенным интерфейсом
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: мониторинг
# Первое изменение: 23.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный

prepare

copy "$source/prometheus" bin/prometheus
copy "$port/prometheus.yml" etc/prometheus/prometheus.yml
copy "$port/service" etc/services/prometheus
copy "$source/LICENSE" usr/share/licenses/prometheus/LICENSE
copy "$source/NOTICE" usr/share/licenses/prometheus/NOTICE
