# recipe.sh: Упаковать проверенный статический выпуск Gitea
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: сервер Git
# Первое изменение: 23.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный

prepare

fetch 'https://dl.gitea.com/gitea/28.2.0/gitea-28.2.0-linux-amd64.xz' \
	'3f3c217eb56fd25e6c8c8bd36c6fc64e2fba5f6e364c30d35b3fec39292679ee' \
	"$work/gitea-linux-amd64.xz"

xz -dc "$work/gitea-linux-amd64.xz" > "$work/gitea"
copy "$work/gitea" bin/gitea
chmod 755 "$payload/bin/gitea"
copy "$port/app.ini" etc/gitea/app.ini
copy "$port/service" etc/services/gitea
copy "$source/LICENSE" usr/share/licenses/gitea/LICENSE
