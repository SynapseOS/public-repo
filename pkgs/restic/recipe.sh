# recipe.sh: Упаковать проверенный статический выпуск restic
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: резервное копирование
# Первое изменение: 23.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный

prepare

fetch 'https://github.com/restic/restic/releases/download/v0.19.1/restic_0.19.1_linux_amd64.bz2' \
	'f415415624dcc452f2a02b8c33641791a8c6d6d3b65bbb3543fcf9a25151585c' \
	"$work/restic-linux-amd64.bz2"

bzip2 -dc "$work/restic-linux-amd64.bz2" > "$work/restic"
copy "$work/restic" bin/restic
chmod 755 "$payload/bin/restic"
copy "$source/LICENSE" usr/share/licenses/restic/LICENSE
