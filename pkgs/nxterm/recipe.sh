# recipe.sh: Собрать закреплённый терминал NanoX
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

source="$work/source"
mkdir -p "$source"

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/downloads/$filename"
	cp "$work/downloads/$filename" "$source/$filename"
done < <(jq -r '.sources[] | [.name,.url,.sha256] | @tsv' "$spec")

while IFS= read -r file; do
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/$file"
done \
	< <(jq -r '.patches[]' "$spec")

core="$SDK/apps/apps/nano-x/upstream/src"
make -C "$core/nanox" "CC=$CC" AR=ar HOSTCC=gcc
"$CC" -O2 -D_GNU_SOURCE -DUNIX=1 -DLINUX=1 -I"$core/include" -I"$source" "$source/nxterm.c" \
	"$core/lib/libnano-X.a" -lm -o "$payload/bin/nxterm"
copy "$SDK/apps/apps/nano-x/upstream/LICENSE" usr/share/licenses/nxterm/LICENSE
