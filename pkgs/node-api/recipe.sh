# recipe.sh: Упаковать тестовый API пилота Skynes
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: серверные пакеты, первое и последнее изменение: 13.11.2026
# Статус доступа: общедоступный

copy "$port/skynes-pilot.js" usr/libexec/skynes-pilot.js
copy "$port/service" etc/services/node-api
