#!/bin/bash
# repository.sh: подготовить подписанный каталог пакетов
# Автор: Елчинян Арен Артурович, 2026, архитектура хоста, ABI POSIX
# Подсистема: выпуск пакетов, первое изменение: 18.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

set -euo pipefail
export LC_ALL=C
: "${ROOT:?}" "${REPO_DIR:?}" "${GENERATION:?}" "${SIGNING_PUBLIC_KEY:?}" "${REPOSITORY_TOOL:?}"
REPO_ID=${REPO_ID:-official}
TTL=${TTL:-2592000}

# Сообщить причину отказа до замены текущего поколения
fail() {
	printf 'repository: %s\n' "$*" >&2
	exit 1
}

# Удалить только временные файлы текущего запуска
cleanup() {
	if [[ -n ${stage:-} && -d $stage ]]; then
		rm -rf -- "$stage"
	fi
}

if [[ ! $REPO_ID =~ ^[a-z0-9][a-z0-9_.-]{0,62}$ ]]; then
	fail 'Invalid repository identifier'
fi

if [[ ! $GENERATION =~ ^[1-9][0-9]{0,14}$ ]]; then
	fail 'Invalid generation'
fi

if [[ ! $TTL =~ ^[1-9][0-9]{0,8}$ ]]; then
	fail 'Invalid TTL in seconds'
fi

if [[ -z ${SSH_AUTH_SOCK:-} || ! -S $SSH_AUTH_SOCK ]]; then
	fail 'Load the signing key into SSH-agent first'
fi

trusted=-

if [[ $REPO_ID == official ]]; then
	trusted="$ROOT/build/official-key.pub"
	selected=$("$REPOSITORY_TOOL" key "$SIGNING_PUBLIC_KEY")
	if [[ $selected != "$(tr -d '\r\n' <"$trusted")" ]]; then
		fail 'Wrong official signing key'
	fi
fi

mkdir -p -- "$REPO_DIR"
REPO_DIR=$(realpath -- "$REPO_DIR")
exec 9>"$REPO_DIR/.publish.lock"
flock -x 9

if [[ -e $REPO_DIR/current.json ]]; then
	previous=$(jq -er '.generation | numbers | select(. >= 1 and . == floor)' "$REPO_DIR/current.json")
	if [[ $GENERATION -le $previous ]]; then
		fail 'Generation must strictly increase'
	fi

	previous_id=$(jq -er .repo "$REPO_DIR/snapshots/$previous/index.json")
	if [[ $previous_id != "$REPO_ID" ]]; then
		fail 'Repository identifier differs from the previous snapshot'
	fi
fi

if [[ -e $REPO_DIR/snapshots/$GENERATION ]]; then
	fail 'Snapshot already exists'
fi

stage=$(mktemp -d "$REPO_DIR/.publish.XXXXXXXX")
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir "$stage/packages" "$stage/snapshot"
: >"$stage/entries.jsonl"

if [[ $# -eq 0 ]]; then
	: "${OUT:?}"
	shopt -s nullglob
	declare -A selected=()
	archives=("$OUT"/*.spkg)
	manifests=("$OUT"/*.manifest.json)

	for archive in "${archives[@]}"; do
		if ! tar --occurrence=1 -xOf "$archive" manifest.json >"$stage/candidate.json"; then
			fail "Cannot read package manifest: $archive"
		fi

		name=$(jq -er '.name | strings | select(test("^[a-z0-9_.-]{1,63}$"))' "$stage/candidate.json")
		manifest="$OUT/$name.manifest.json"

		if [[ ! -f $manifest ]]; then
			continue
		fi

		if ! jq -e --slurpfile current "$manifest" '. == $current[0]' "$stage/candidate.json" >/dev/null; then
			continue
		fi

		if [[ -n ${selected[$name]:-} ]]; then
			fail "Multiple archives match the current manifest: $name, set PACKAGE_FILES explicitly"
		fi

		selected[$name]=$archive
		set -- "$@" "$archive"
	done

	for manifest in "${manifests[@]}"; do
		name=$(basename "$manifest" .manifest.json)

		if [[ -z ${selected[$name]:-} ]]; then
			fail "No archive matches the current manifest: $name"
		fi
	done
fi

if [[ $# -eq 0 || $# -gt 512 ]]; then
	fail 'Select between 1 and 512 package archives'
fi

for package in "$@"; do
	if [[ ! -f $package || -L $package ]]; then
		fail "Not a regular package: $package"
	fi

	if [[ $(stat -c %s -- "$package") -gt 268435456 ]]; then
		fail "Package too large: $package"
	fi

	cp -- "$package" "$stage/input.spkg"
	"$REPOSITORY_TOOL" inspect "$stage/input.spkg" >"$stage/inspection.json"
	jq '.manifest = (.manifest_text | fromjson) | del(.manifest_text)' \
		"$stage/inspection.json" >"$stage/parsed.json"
	mv -- "$stage/parsed.json" "$stage/inspection.json"

	if ! jq -e '
		def safe_path:
			type == "string" and length < 512 and
			test("^[!-~]+$") and (contains("\\") | not) and (contains("\"") | not) and
			(split("/") | all(. != "" and . != "." and . != "..")) and
			test("^(bin/|lib/|etc/|usr/share/|usr/lib/|usr/include/|usr/libexec/)") and
			(test("^etc/(setup/|dropbear/|settings\\.ini$|shadow$|passwd$)") | not);
		def integer: type == "number" and . >= 0 and . == floor;
		.manifest as $m |
		($m | keys == ["abi","arch","conflicts","depends","description","files","format","kind","name","version"]) and
		$m.format == 1 and $m.arch == "x86_64" and $m.abi == "synapse-x86_64-v1" and
		($m.name | type == "string" and test("^[a-z0-9_.-]{1,63}$")) and
		($m.version | type == "string" and test("^[0-9]+\\.[0-9]+\\.[0-9]+-r[0-9]+$")) and
		($m.description | type == "string") and
		($m.kind | . == "application" or . == "library" or . == "tool" or . == "kernel") and
		([$m.depends, $m.conflicts] | all(type == "array" and length <= 32 and all(type == "string"))) and
		($m.files | type == "array" and length <= 4096 and
			(length == (map(.path) | unique | length)) and all(
				(.path | safe_path) and (.type == "file" or .type == "directory") and
				(.mode | integer and . <= 511) and (.size | integer) and
				(.sha256 | type == "string" and test("^[0-9a-f]{64}$")) and
				(.config | type == "boolean"))) and
		([.entries[] | {path,type,mode,size,sha256}] | sort_by(.path)) ==
		([$m.files[] | {path,type,mode,size,sha256}] | sort_by(.path))
	' "$stage/inspection.json" >/dev/null; then
		fail "Invalid package: $package"
	fi

	name=$(jq -er .manifest.name "$stage/inspection.json")

	if [[ $REPO_ID == official ]]; then
		if [[ ! -f $ROOT/pkgs/$name/manifest.json ]]; then
			fail "Package is not in the public package set: $name"
		fi
	fi

	hash=$(jq -er .sha256 "$stage/inspection.json")
	jq -c '{manifest,sha256,size,url:("packages/" + .sha256 + ".spkg")}' \
		"$stage/inspection.json" >>"$stage/entries.jsonl"
	mv -- "$stage/input.spkg" "$stage/packages/$hash.spkg"
done

jq -s 'sort_by(.manifest.name,.manifest.version,.manifest.arch,.manifest.abi)' \
	"$stage/entries.jsonl" >"$stage/packages.json"
if ! jq -e 'length == (unique_by(.manifest | [.name,.version,.arch,.abi]) | length)' \
	"$stage/packages.json" >/dev/null; then
	fail 'Duplicate package version'
fi

shopt -s nullglob

for old_index in "$REPO_DIR"/snapshots/*/index.json; do
	if ! jq -e --slurpfile current "$stage/packages.json" '
		all(.packages[]; . as $old | all($current[0][];
			.manifest as $new |
			([$new.name,$new.version,$new.arch,$new.abi] !=
			 [$old.manifest.name,$old.manifest.version,$old.manifest.arch,$old.manifest.abi]) or
			.sha256 == $old.sha256))
	' "$old_index" >/dev/null; then
		fail 'Changed content requires a new package version'
	fi
done

created=$(date +%s)
expires=$((created + TTL))
jq -cn --arg repo "$REPO_ID" --argjson generation "$GENERATION" \
	--argjson created "$created" --argjson expires "$expires" \
	--slurpfile packages "$stage/packages.json" \
	'{format:2,repo:$repo,generation:$generation,created:$created,expires:$expires,packages:$packages[0]}' \
	>"$stage/snapshot/index.json"

if [[ $(stat -c %s "$stage/snapshot/index.json") -gt 4194304 ]]; then
	fail 'Repository index exceeds the setup metadata limit'
fi

"$REPOSITORY_TOOL" sign "$SIGNING_PUBLIC_KEY" "$trusted" \
	"$stage/snapshot/index.json" "$stage/snapshot/index.sig"
"$REPOSITORY_TOOL" verify "$SIGNING_PUBLIC_KEY" \
	"$stage/snapshot/index.json" "$stage/snapshot/index.sig"

mkdir -p "$REPO_DIR/packages" "$REPO_DIR/snapshots"

for package in "$stage"/packages/*.spkg; do
	destination="$REPO_DIR/packages/$(basename "$package")"

	if [[ -e $destination ]]; then
		if ! cmp -- "$package" "$destination"; then
			fail 'Existing immutable archive differs'
		fi
	else
		chmod 644 "$package"
		ln -- "$package" "$destination"
	fi
done

chmod 755 "$stage/snapshot"
chmod 644 "$stage/snapshot/index.json" "$stage/snapshot/index.sig"
mv -T -- "$stage/snapshot" "$REPO_DIR/snapshots/$GENERATION"
jq -cn --argjson generation "$GENERATION" '{format:1,generation:$generation}' >"$stage/current.json"
chmod 644 "$stage/current.json"
sync -f "$REPO_DIR"
mv -T -- "$stage/current.json" "$REPO_DIR/current.json"
sync -f "$REPO_DIR"
printf 'Published generation %s in %s\n' "$GENERATION" "$REPO_DIR"
