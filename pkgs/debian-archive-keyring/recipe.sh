# recipe.sh: установить только открытые ключи подписей Debian
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

archive="$work/debian-archive-keyring_2025.1_all.deb"
curl --fail --location --retry 3 \
	--output "$archive" \
	https://deb.debian.org/debian/pool/main/d/debian-archive-keyring/debian-archive-keyring_2025.1_all.deb
printf '%s  %s\n' \
	'9ea7778e443144ca490668737a8ab22dd3e748bb99e805e22ec055abeb3c7fac' \
	"$archive" | sha256sum --check

mkdir -p "$work/keyring-root"
bsdtar -xf "$archive" -C "$work" data.tar.xz
bsdtar -xf "$work/data.tar.xz" -C "$work/keyring-root"

copy "$work/keyring-root/usr/share/keyrings/debian-archive-keyring.pgp" \
	usr/share/keyrings/debian-archive-keyring.pgp
copy "$work/keyring-root/usr/share/doc/debian-archive-keyring/copyright" \
	usr/share/licenses/debian-archive-keyring/copyright
