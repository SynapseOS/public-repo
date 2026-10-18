# recipe.sh: Собрать панель и сохранить клиентские ресурсы
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

"$CC" -O2 -I"$SDK/apps/apps/common" "$port/main.c" -lcrypt -o "$payload/bin/webpanel"

for file in index.html app.js style.css; do
	copy "$port/files/$file" "usr/share/webpanel/$file"
done

copy "$port/files/service" etc/services/webpanel
copy "$port/files/nginx.conf.example" etc/webpanel/nginx.conf.example
