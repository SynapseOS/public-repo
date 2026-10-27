# recipe.sh: Собрать Caddy и проверить зависимости по upstream go.sum
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: сетевые серверы, первое изменение: 22.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный

prepare
export CGO_ENABLED=0 GOOS=linux GOARCH=amd64 GOAMD64=v1 GOTOOLCHAIN=local
export GOCACHE="$BUILD_ROOT/go/cache" GOMODCACHE="$work/modules" GOMAXPROCS="$JOBS"
go="$BUILD_ROOT/go/go/bin/go"
(
	cd "$source"
	"$go" mod download
	"$go" mod verify
	"$go" build -p "$JOBS" -mod=readonly -trimpath -buildvcs=false \
		-ldflags='-s -w -X github.com/caddyserver/caddy/v2.CustomVersion=v2.11.7' \
		-o "$payload/bin/caddy" ./cmd/caddy
)
copy "$source/LICENSE" usr/share/licenses/caddy/LICENSE
copy "$port/Caddyfile" etc/caddy/Caddyfile
copy "$port/service" etc/services/caddy

# Лицензии модулей сохраняются рядом с лицензией основного проекта
(
	cd "$source"
	"$go" list -m -json all
) > "$work/modules.json"

while IFS=$'\t' read -r module directory; do
	[[ -d $directory && $module != *..* && $module != /* ]]

	while IFS= read -r -d '' file; do
		copy "$file" "usr/share/licenses/caddy/modules/$module/$(basename "$file")"
	done < <(find "$directory" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) -print0)
done < <(jq -r 'select(.Path != "github.com/caddyserver/caddy/v2" and .Dir != null) | [.Path,.Dir] | @tsv' "$work/modules.json")
