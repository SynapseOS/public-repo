# recipe.sh: Упаковать закреплённые бинарники 3x-ui и Xray-core
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: серверные пакеты, первое и последнее изменение: 14.11.2026
# Статус доступа: общедоступный

fetch "$(jq -er .source "$spec")" "$(jq -er .sha256 "$spec")" "$work/source.tar.gz"
fetch "$(jq -er .license_source "$spec")" "$(jq -er .license_sha256 "$spec")" "$work/LICENSE"

tar -xzf "$work/source.tar.gz" -C "$work" \
    x-ui/x-ui x-ui/bin/xray-linux-amd64 x-ui/bin/geoip.dat x-ui/bin/geosite.dat x-ui/bin/LICENSE

install -Dm755 "$port/launcher" "$payload/bin/3x-ui"
install -Dm755 "$work/x-ui/x-ui" "$payload/usr/libexec/3x-ui/x-ui"
install -Dm755 "$work/x-ui/bin/xray-linux-amd64" "$payload/usr/libexec/3x-ui/xray-linux-amd64"
copy "$work/x-ui/bin/geoip.dat" usr/libexec/3x-ui/geoip.dat
copy "$work/x-ui/bin/geosite.dat" usr/libexec/3x-ui/geosite.dat
copy "$work/LICENSE" usr/share/licenses/3x-ui/LICENSE
copy "$work/x-ui/bin/LICENSE" usr/share/licenses/3x-ui/Xray-LICENSE
copy "$port/service" etc/services/3x-ui
copy "$port/README.md" usr/share/help/3x-ui.md
