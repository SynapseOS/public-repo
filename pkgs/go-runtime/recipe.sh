# recipe.sh: Установить стандартную библиотеку для linux amd64 без cgo
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: Go, первое изменение: 22.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

source_tree="$BUILD_ROOT/go/go"
go="$source_tree/bin/go"
export CGO_ENABLED=0 GOOS=linux GOARCH=amd64 GOAMD64=v1 GOTOOLCHAIN=local
export GOCACHE="$BUILD_ROOT/go/cache" GOMODCACHE="$BUILD_ROOT/go/modules" GOMAXPROCS="$JOBS"
"$go" list -deps -json std > "$work/stdlib.json"

while IFS=$'\t' read -r directory file; do
	relative=${directory#"$source_tree/"}
	[[ $relative != "$directory" && $relative == src/* && $file != /* && $file != *../* ]]
	copy "$directory/$file" "usr/lib/go/$relative/$file"
done < <(jq -r '[.Dir] as $dir | ((.GoFiles // [])[], (.SFiles // [])[], (.HFiles // [])[], (.SysoFiles // [])[], (.EmbedFiles // [])[], (.CFiles // [])[]) | $dir + [.] | @tsv' "$work/stdlib.json")

while IFS= read -r -d '' file; do
	copy "$file" "usr/lib/go/${file#"$source_tree/"}"
done < <(find "$source_tree/src" -type f \( -name go.mod -o -name go.sum \) -print0)

for file in VERSION go.env LICENSE PATENTS; do
	copy "$source_tree/$file" "usr/lib/go/$file"
done

for file in "$source_tree"/pkg/include/*.h; do
	copy "$file" "usr/lib/go/pkg/include/$(basename "$file")"
done

copy "$source_tree/lib/time/zoneinfo.zip" usr/lib/go/lib/time/zoneinfo.zip
copy "$source_tree/LICENSE" usr/share/licenses/go-runtime/LICENSE

# Сохранить условия использования встроенных сторонних исходников
while IFS= read -r -d '' file; do
	copy "$file" "usr/lib/go/${file#"$source_tree/"}"
done < <(find "$source_tree/src/vendor" -type f \( -name 'LICENSE*' -o -name 'COPYING*' -o -name 'NOTICE*' -o -name modules.txt \) -print0)

# Заголовок включается ассемблером runtime также при выключенном cgo
copy "$source_tree/src/runtime/cgo/abi_amd64.h" usr/lib/go/src/runtime/cgo/abi_amd64.h
