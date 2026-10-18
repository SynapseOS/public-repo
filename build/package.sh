#!/bin/bash
# package.sh: сборка пакета по закреплённому рецепту
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

set -euo pipefail
source "$ROOT/build/common.sh"
export SYNAPSE_LINK_MODE=static
name=${1:?Package name required}

case "$name" in
	nmap-nse-* | nmap-nselib | open-posix-testsuite-data-* | wayland-client | xkeyboard-config)
		test -s "$OUT/$name.manifest.json"
		exit 0
		;;
esac

[[ $name =~ ^[a-z0-9_-]+$ ]]
port="$ROOT/pkgs/$name"
spec="$port/port.json"
work="$BUILD_ROOT/$name"
payload="$work/payload"

mkdir -p "$work"
exec 9>"$work/build.lock"
flock 9
rm -rf -- "$payload"
mkdir -p "$payload/bin"
CC="$SDK/apps/apps/common/musl-cc"
export CC

if [[ -f $ROOT/pkgs/$name/recipe.sh ]]; then
	source "$ROOT/pkgs/$name/recipe.sh"
	pack
	exit
fi

prepare

# Старые {root} пути рецептов продолжают обозначать общий корень SDK и пакетов
substitute() {
	local text=$1
	text=${text//\{root\}/$ROOT}
	text=${text//\{gcc_headers\}/$(gcc -print-file-name=include)}
	printf '%s' "$text"
}

# Прочитать массив аргументов без eval и потери пробелов
args() {
	local field=$1 item
	arguments=()

	while IFS= read -r -d '' item; do
		arguments+=("$(substitute "$item")")
	done < <(jq -j "$field | .[] | ., \"\\u0000\"" "$spec")
}

flags=()
args '.cflags // ["-O2", "-D__synapse__"]'
flags=("${arguments[@]}")
export CFLAGS LDFLAGS PKG_CONFIG=false
printf -v CFLAGS '%q ' "${flags[@]}"
LDFLAGS='-static -Wl,--gc-sections'

while IFS=$'\t' read -r key value; do
	[[ $key =~ ^[a-zA-Z_][a-zA-Z_0-9]*$ ]]
	export "$key=$(substitute "$value")"
done < <(jq -r '.configure_environment // {} | to_entries[] | [.key,.value] | @tsv' "$spec")

if jq -e '.configure' "$spec" >/dev/null; then
	args '.configure'

	(
		cd "$source" &&
			"${arguments[@]}"
	)

	args '.make_targets'
	make -C "$source" -j"$JOBS" "${arguments[@]}"

	while IFS=$'\t' read -r destination origin; do
		if [[ -d $source/$origin ]]; then
			while IFS= read -r -d '' file; do
				copy "$file" "$destination/${file#"$source/$origin/"}"
			done \
				< <(find "$source/$origin" -type f -name '*.h' -print0)
		else
			copy "$source/$origin" "$destination"
		fi
	done < <(jq -r '.files | to_entries[] | [.key,.value] | @tsv' "$spec")
else
	system=$(jq -r '.build_system // "direct"' "$spec")

	case "$system" in
		configure)
			args '.configure_args'

			(
				cd "$source" &&
					./configure "${arguments[@]}"
			)
			;;
		make | direct)
			;;
		*)
			echo "Unknown build system: $system" >&2
			exit 1
			;;
	esac

	if [[ $system == direct ]]; then
		args '.sources'

		(
			cd "$source" &&
				"$CC" "${flags[@]}" "${arguments[@]}" -Wl,--gc-sections \
					-o "$payload/bin/$(jq -r '.binary // .name' "$spec")"
		)
	else
		args '.make_args // []'
		make_args=("${arguments[@]}")
		args '.make_targets // []'
		make -C "$source" -j"$JOBS" "${make_args[@]}" "${arguments[@]}"

		while IFS=$'\t' read -r destination origin; do
			copy "$source/$origin" "$destination"
		done \
			< <(jq -r '. as $s | (.binaries // {("bin/"+(.binary // .name)):
                (.binary_source // .binary // .name)}) | to_entries[] | [.key,.value] | @tsv' "$spec")
	fi
fi

if jq -e .license_file "$spec" >/dev/null; then
	license=$(jq -r .license_file "$spec")
	copy "$source/$license" "usr/share/licenses/$name/$(basename "$license")"
fi

while IFS=$'\t' read -r destination origin; do
	copy "$ROOT/$origin" "$destination"
done \
	< <(jq -r '.additional_files // {} | to_entries[] | [.key,.value] | @tsv' "$spec")

while IFS= read -r -d '' binary; do
	if readelf -l -d "$binary" 2>/dev/null | grep -Eq 'INTERP|\(NEEDED\)'; then
		echo "Unexpected dynamic executable: $binary" >&2
		exit 1
	fi

	chmod 755 "$binary"
done < <(find "$payload/bin" -type f -print0)

pack
