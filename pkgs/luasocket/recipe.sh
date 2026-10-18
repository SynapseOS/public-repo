# recipe.sh: Собрать динамические модули для Lua 5.5
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

jq -n '{source:"https://codeload.github.com/lunarmodules/luasocket/tar.gz/refs/tags/v3.1.0",
    sha256:"bf033aeb9e62bcaa8d007df68c119c966418e8c9ef7e4f2d7e96bddeca9cca6e",
    source_directory:"luasocket-3.1.0",patches:["patches/0001-monotonic-timeouts.patch"]}' >"$work/source.json"
spec="$work/source.json"
prepare
base=(luasocket timeout buffer io auxiliar compat options inet usocket except select tcp udp)

for module in socket mime; do
	files=(mime compat)

	if [[ $module == socket ]]; then
		files=("${base[@]}")
	fi

	arguments=()

	for file in "${files[@]}"; do
		arguments+=("$source/src/$file.c")
	done

	mkdir -p "$payload/usr/lib/lua/5.5/$module"
	"$CC" -shared -O2 -DLUASOCKET_DEBUG -I"$SDK/apps/apps/lua" "${arguments[@]}" \
		-o "$payload/usr/lib/lua/5.5/$module/core.so"
done

for file in "$source/src/"*.lua; do
	filename=$(basename "$file")

	case "$filename" in
		socket.lua | mime.lua | ltn12.lua)
			destination=$filename
			;;
		*)
			destination=socket/$filename
			;;
	esac

	copy "$file" "usr/share/lua/5.5/$destination"
done

copy "$source/LICENSE" usr/share/licenses/luasocket/LICENSE
copy "$port/help.md" usr/share/help/luasocket.md
