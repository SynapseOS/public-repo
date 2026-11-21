# recipe.sh: Собрать пакет панели 3x-ui и Xray-core
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: серверные пакеты, первое изменение: 14.11.2026, последнее изменение: 20.11.2026
# Статус доступа: общедоступный

fetch "$(jq -er .source "$spec")" "$(jq -er .sha256 "$spec")" "$work/source.tar.gz"
fetch "$(jq -er .license_source "$spec")" "$(jq -er .license_sha256 "$spec")" "$work/LICENSE"

# Обе архитектуры используют одни и те же базы адресов из закреплённого выпуска
tar -xzf "$work/source.tar.gz" -C "$work" x-ui/bin/geoip.dat x-ui/bin/geosite.dat x-ui/bin/LICENSE
platform=amd64
if [[ $TARGET_ARCH == aarch64 ]]; then
    platform=arm64
    fetch "$(jq -er .panel_source "$spec")" "$(jq -er .panel_sha256 "$spec")" "$work/panel-source.tar.gz"
    fetch "$(jq -er .xray_source "$spec")" "$(jq -er .xray_sha256 "$spec")" "$work/xray-source.tar.gz"
    tar -xzf "$work/panel-source.tar.gz" -C "$work"
    tar -xzf "$work/xray-source.tar.gz" -C "$work"
    export GOTOOLCHAIN=local GOTELEMETRY=off GOMODCACHE="$BUILD_ROOT/go-mod" GOCACHE="$BUILD_ROOT/go-cache"
    (
        cd "$work/3x-ui-3.11.0/frontend"
        npm ci --cache "$BUILD_ROOT/npm-cache" --no-audit --no-fund
        X_UI_VERSION=$(jq -er .upstream_version "$spec") npm run build
    )
    (
        cd "$work/3x-ui-3.11.0"
        GOOS=linux GOARCH=arm64 CGO_ENABLED=1 go build -p "$JOBS" -mod=readonly -trimpath -buildvcs=false \
            -ldflags="-s -w -linkmode external -extldflags '-static'" -o "$work/x-ui/x-ui" .
    )
    (
        cd "$work/Xray-core-26.10.21"
        GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -p "$JOBS" -mod=readonly -trimpath -buildvcs=false \
            -ldflags='-s -w -R 4096 -T 0x10001000' -o "$work/x-ui/bin/xray-linux-arm64" ./main
    )
else
    tar -xzf "$work/source.tar.gz" -C "$work" x-ui/x-ui x-ui/bin/xray-linux-amd64
fi

install -Dm755 "$port/launcher" "$payload/bin/3x-ui"
install -Dm755 "$work/x-ui/x-ui" "$payload/usr/libexec/3x-ui/x-ui"
install -Dm755 "$work/x-ui/bin/xray-linux-$platform" "$payload/usr/libexec/3x-ui/xray-linux-$platform"
copy "$work/x-ui/bin/geoip.dat" usr/libexec/3x-ui/geoip.dat
copy "$work/x-ui/bin/geosite.dat" usr/libexec/3x-ui/geosite.dat
copy "$work/LICENSE" usr/share/licenses/3x-ui/LICENSE
copy "$work/x-ui/bin/LICENSE" usr/share/licenses/3x-ui/Xray-LICENSE
copy "$port/service" etc/services/3x-ui
copy "$port/README.md" usr/share/help/3x-ui.md
