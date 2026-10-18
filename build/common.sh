#!/bin/bash
# common.sh: загрузка, подготовка и упаковка публичных пакетов
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

set -euo pipefail
export LC_ALL=C.UTF-8
: "${ROOT:?}" "${BUILD_ROOT:?}" "${OUT:?}" "${SDK:?}"

# Загрузить закреплённый файл, проверить также ранее загруженные данные
fetch() {
	local url=$1 sum=$2 dest=$3
	[[ $sum =~ ^[a-f0-9]{64}$ ]] ||
		{
			echo 'Invalid SHA-256' >&2
			return 1
		}
	mkdir -p "$(dirname "$dest")"

	if [[ ! -f $dest ]]; then
		curl --fail --location --connect-timeout 20 --max-time 900 --retry 3 \
			--proto '=https' --proto-redir '=https' "$url" -o "$dest.download"
		printf '%s  %s\n' "$sum" "$dest.download" | sha256sum --check --status
		mv "$dest.download" "$dest"
	fi

	printf '%s  %s\n' "$sum" "$dest" | sha256sum --check --status
}

# Скопировать обычный файл с разыменованием ссылки
copy() {
	mkdir -p "$(dirname "$payload/$2")"
	cp -L -- "$1" "$payload/$2"
}

# Подготовить закреплённые исходники в отдельном каталоге пакета
prepare() {
	local archive="$work/source.tar" patch directory
	fetch "$(jq -er .source "$spec")" "$(jq -er .sha256 "$spec")" "$archive"
	directory=$(jq -er .source_directory "$spec")
	[[ $directory =~ ^[a-zA-Z0-9_.+-]+$ && $directory != . && $directory != .. ]]
	source="$work/$directory"
	[[ $source == "$work/"* && $source != *'/../'* ]]
	rm -rf "$work/source-new"
	mkdir -p "$work/source-new"
	bsdtar -xf "$archive" -C "$work/source-new"
	[[ -d "$work/source-new/$(basename "$source")" ]]
	rm -rf -- "$source"
	mv "$work/source-new/$(basename "$source")" "$source"
	rmdir "$work/source-new"

	while IFS= read -r patch; do
		patch --binary --batch --fuzz=0 -d "$source" -p1 -i "$port/$patch"
	done < <(jq -r '.patches // [] | .[]' "$spec")
}

# Сформировать совместимый архив USTAR с manifest.json первым элементом
pack() {
	local name=${1:-$name} tree=${2:-$payload} manifest=${3:-$port/manifest.json}
	local stage="$work/pack-$name" rel mode config digest required
	[[ -d $tree && ! -L $tree ]]

	if [[ -n $(find "$tree" ! -type f ! -type d -print -quit) ]]; then
		echo 'Unsupported payload object, only regular files and directories are allowed' >&2
		return 1
	fi

	if [[ -f $SDK/docs/user/$name.md ]]; then
		mkdir -p "$tree/usr/share/help"
		cp "$SDK/docs/user/$name.md" "$tree/usr/share/help/$name.md"
	fi

	mkdir -p "$tree/usr/share/licenses/$name"
	cp "$SDK/apps/musl/COPYRIGHT" "$tree/usr/share/licenses/$name/musl-COPYRIGHT"

	while IFS= read -r required; do
		[[ -f $tree/$required ]] ||
			{
				echo "Missing declared executable: $required" >&2
				return 1
			}
	done < <(jq -r '.files[] | select(.type=="file" and (.path|startswith("bin/"))) | .path' "$manifest")

	mkdir -p "$stage/payload" "$OUT"
	: >"$stage/files.jsonl"
	: >"$stage/order"

	while IFS= read -r -d '' file; do
		rel=${file#"$tree/"}

		if [[ $(od -An -tx1 -N4 "$file" | tr -d ' \n') == 7f454c46 ]]; then
			if readelf -d "$file" 2>/dev/null |
				grep -E '\[(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|ld-linux[^]]*)\]' >/dev/null; then
				echo "Host glibc dependency in $rel" >&2
				return 1
			fi
		fi

		[[ $rel != *$'\n'* && $rel != *'/../'* && $rel != /* ]]

		case "$rel" in
			bin/* | lib/* | etc/* | usr/share/* | usr/lib/* | usr/include/* | usr/libexec/*)
				;;
			*)
				return 1
				;;
		esac

		case "$rel" in
			etc/setup/* | etc/dropbear/* | etc/settings.ini | etc/shadow | etc/passwd)
				return 1
				;;
		esac

		mode=420

		if [[ -x $file ]]; then
			mode=493
		fi

		config=false

		if [[ $rel == etc/* ]]; then
			config=true
		fi

		mkdir -p "$(dirname "$stage/payload/$rel")"
		cp -- "$file" "$stage/payload/$rel"
		chmod "$(printf '%o' "$mode")" "$stage/payload/$rel"
		# Ненавижу это место
		jq -cn --arg path "$rel" --arg sha "$(sha256sum "$file" | cut -d' ' -f1)" \
			--argjson size "$(stat -c %s "$file")" --argjson mode "$mode" --argjson config "$config" \
			'{path:$path,type:"file",mode:$mode,config:$config,size:$size,sha256:$sha}' >>"$stage/files.jsonl"
		printf 'payload/%s\n' "$rel" >>"$stage/order"
	done < <(find "$tree" -type f -print0 | sort -z)

	if [[ $(wc -l <"$stage/files.jsonl") -gt 512 ]]; then
		echo "Package $name exceeds the 512-file manifest limit" >&2
		return 1
	fi

	jq -cs '.' "$stage/files.jsonl" >"$stage/files.json"
	jq -c --slurpfile files "$stage/files.json" '.files=$files[0]' "$manifest" >"$stage/manifest.json"
	jq -e '.format==1 and .arch=="x86_64" and .abi=="synapse-x86_64-v1" and
        (.name|test("^[a-z0-9_.-]{1,63}$")) and
        (.version|test("^[0-9]+\\.[0-9]+\\.[0-9]+-r[0-9]+$")) and
        (.kind=="application" or .kind=="library" or .kind=="tool" or .kind=="kernel") and
        (.depends|type=="array" and length<=32) and (.conflicts|type=="array" and length<=32)' \
		"$stage/manifest.json" >/dev/null

	{
		printf 'manifest.json\n'
		cat "$stage/order"
	} >"$stage/archive-order"

	tar --format=ustar --mtime=@0 --owner=0 --group=0 --numeric-owner --no-recursion \
		-C "$stage" -cf "$stage/package.tar" -T "$stage/archive-order"
	[[ $(stat -c %s "$stage/package.tar") -le 268435456 ]]
	digest=$(sha256sum "$stage/package.tar" | cut -d' ' -f1)

	if [[ -e $OUT/$digest.spkg ]]; then
		cmp "$stage/package.tar" "$OUT/$digest.spkg"
	else
		mv "$stage/package.tar" "$OUT/$digest.spkg"
	fi

	cp "$stage/manifest.json" "$OUT/$name.manifest.json"
	printf '%s\n' "$OUT/$digest.spkg"
}

# Включить замыкание общих библиотек, не подмешивая libc хоста
runtime_deps() {
	local destination=$1
	shift
	local binary library candidate file
	local -a pending=()
	local -A seen=()

	while IFS= read -r -d '' file; do
		if [[ $(od -An -tx1 -N4 "$file" | tr -d ' \n') == 7f454c46 ]]; then
			pending+=("$file")
		fi
	done < <(find "$payload" -type f -print0)

	while ((${#pending[@]})); do
		binary=${pending[0]}
		pending=("${pending[@]:1}")

		while IFS= read -r library; do
			case "$library" in
				libc.so | libc.musl-* | libSDL2-2.0.so.0)
					continue
					;;
				libc.so.6 | ld-linux* | libpthread.so.0)
					return 1
					;;
			esac

			[[ ${seen[$library]:-} == yes ]] &&
				continue
			seen[$library]=yes
			candidate=$(find "$payload" -type f -name "$library" -print -quit)

			if [[ -z $candidate ]]; then
				for directory; do
					if [[ -f $directory/$library ]]; then
						candidate="$directory/$library"
						break
					fi

					candidate=$(find "$directory" -type f -name "$library.*" -print -quit)
					[[ -n $candidate ]] &&
						break
				done

				[[ -n $candidate && -f $candidate ]] ||
					{
						echo "Missing runtime $library" >&2
						return 1
					}
				copy "$candidate" "$destination/$library"
				candidate="$payload/$destination/$library"
			fi

			pending+=("$candidate")
		done < <(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')
	done
}

# Создать обычные правила Make для независимой компиляции набора исходников
compile_objects() {
	local directory=$1
	shift
	local compiler file object language
	local rules="$directory/objects.mk"
	local pending_rules="$directory/objects.mk.new"
	local -a flags=("$@")
	objects=()
	mkdir -p "$directory"
	: >"$pending_rules"

	for file in "${sources[@]}"; do
		object="$directory/$(printf '%s' "$file" | sha256sum | cut -c1-16).o"
		objects+=("$object")
		compiler="$CC"
		language=-std=c11

		if [[ $file != *.c ]]; then
			compiler="$BUILD_ROOT/mesa/cxx"
			language=-std=c++11
		fi

		printf '%s: %s %s\n\t' "$object" "$file" "$rules" >>"$pending_rules"
		printf '%q ' "$compiler" -O2 -fPIC -fvisibility=hidden "$language" "${flags[@]}" \
			-MMD -MP -MF "$object.d" -c "$file" -o "$object" >>"$pending_rules"
		printf '\n' >>"$pending_rules"
	done

	printf 'all: %s\n' "${objects[*]}" >>"$pending_rules"
	printf '%s\n' "-include $directory/*.d" >>"$pending_rules"

	if ! cmp -s "$pending_rules" "$rules"; then
		mv "$pending_rules" "$rules"
	fi

	make -f "$rules" -j"$JOBS" all
}
