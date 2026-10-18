# recipe.sh: Этот пакет задаёт стабильную зависимость от поддерживаемой Java
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

mkdir -p "$payload/usr/share/doc/openjdk-current"
printf 'OpenJDK 21 runtime, run java -version\n' >"$payload/usr/share/doc/openjdk-current/README.md"
