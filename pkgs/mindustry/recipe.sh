# recipe.sh: Сохранить закреплённые официальные JAR и собрать команды запуска
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/downloads/$filename"
	copy "$work/downloads/$filename" "usr/share/games/mindustry/$filename"
done < <(jq -r '.assets[] | [.name,.url,.sha256] | @tsv' "$spec")

for binary in mindustry mindustry-server; do
	server=0

	if [[ $binary == mindustry-server ]]; then
		server=1
	fi

	"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none -DMINDUSTRY_SERVER="$server" \
		"$port/launch.c" -o "$payload/bin/$binary"
done

copy "$port/files/LICENSE" usr/share/licenses/mindustry/LICENSE
copy "$port/help.md" usr/share/doc/mindustry/README.md
