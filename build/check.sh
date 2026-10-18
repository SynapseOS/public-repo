#!/bin/bash
# check.sh: проверить полноту рецептов и входных метаданных
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: проверки сборки пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

set -euo pipefail
count=0

for manifest in "$ROOT"/pkgs/*/manifest.json; do
	directory=$(dirname "$manifest")
	name=$(basename "$directory")
	jq -e --arg name "$name" '.name==$name and .format==1 and .arch=="x86_64" and .abi=="synapse-x86_64-v1"' \
		"$manifest" >/dev/null
	[[ -f $directory/Makefile ]]

	if [[ ! -f $directory/recipe.sh ]]; then
		case "$name" in
			nmap-nse-* | nmap-nselib | open-posix-testsuite-data-* | wayland-client | xkeyboard-config)
				;;
			*)
				jq -e '.source and (.sha256|test("^[0-9a-f]{64}$")) and .source_directory and
                    ((.build_system // "direct") | .=="configure" or .=="make" or .=="direct")' \
					"$directory/port.json" >/dev/null
				;;
		esac
	fi

	count=$((count + 1))
done

if find "$ROOT/pkgs" -path '*/.work' -prune -o -type f \
	\( -name 'build*.py' -o -name '*support.py' -o -name 'toolchain.py' -o -name 'install_pip.py' \) \
	-print | grep -q .; then
	echo 'Python build helper remains in public recipes' >&2
	exit 1
fi

printf 'PACKAGE_RECIPES_CHECK_PASS %s\n' "$count"
