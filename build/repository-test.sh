#!/bin/bash
# repository-test.sh: проверить подпись без ключей владельца и без сети
# Автор: Елчинян Арен Артурович, 2026, архитектура хоста, ABI POSIX
# Подсистема: проверки выпуска, первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

set -euo pipefail
: "${ROOT:?}" "${BUILD_ROOT:?}" "${REPOSITORY_TOOL:?}"
mkdir -p "$BUILD_ROOT"
test_dir=$(mktemp -d "$BUILD_ROOT/repository-test.XXXXXXXX")
agent_pid=

# Завершить только тестовый агент и удалить только временный каталог проверки
cleanup() {
	if [[ -n $agent_pid ]]; then
		kill "$agent_pid" 2>/dev/null || true
		wait "$agent_pid" 2>/dev/null || true
	fi

	rm -rf -- "$test_dir"
}

# Проверить, что запрещённая операция завершилась отказом
reject() {
	if "$@" >"$test_dir/rejection.log" 2>&1; then
		echo 'Expected rejection did not happen' >&2
		exit 1
	fi

	if grep -Eq 'AddressSanitizer|runtime error:|LeakSanitizer' "$test_dir/rejection.log"; then
		cat "$test_dir/rejection.log" >&2
		exit 1
	fi
}

# Преобразовать проверенную шестнадцатеричную строку в байты
unhex() {
	local pair

	while IFS= read -r -N 2 pair; do
		[[ $pair =~ ^[0-9a-f]{2}$ ]]
		printf '%b' "\\x$pair"
	done
}

# Собрать небольшой пакет с известным содержимым
package() {
	local text=$1 version=$2 destination=$3
	printf '%s' "$text" >"$test_dir/payload/bin/signing-test"
	chmod 644 "$test_dir/payload/bin/signing-test"
	local hash size
	hash=$(sha256sum "$test_dir/payload/bin/signing-test" | cut -d' ' -f1)
	size=$(stat -c %s "$test_dir/payload/bin/signing-test")
	jq -cn --arg hash "$hash" --arg version "$version" --argjson size "$size" '
		{format:1,name:"signing-test",version:$version,description:"Проверка подписи",kind:"tool",
		 arch:"x86_64",abi:"synapse-x86_64-v1",depends:[],conflicts:[],files:[
		 {path:"bin/signing-test",type:"file",mode:420,config:false,size:$size,sha256:$hash}]}' \
		>"$test_dir/manifest.json"
	tar --format=ustar --mtime=@0 --owner=0 --group=0 --numeric-owner \
		-C "$test_dir" -cf "$destination" manifest.json payload/bin/signing-test
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export SSH_AUTH_SOCK="$test_dir/agent.sock"
ssh-agent -D -a "$SSH_AUTH_SOCK" >"$test_dir/agent.log" 2>&1 &
agent_pid=$!

for ((attempt = 0; attempt < 50; attempt++)); do
	if [[ -S $SSH_AUTH_SOCK ]]; then
		break
	fi

	sleep 0.1
done

ssh-keygen -q -t ed25519 -N '' -f "$test_dir/key"
ssh-add "$test_dir/key" 2>"$test_dir/ssh-add.log"
export SIGNING_PUBLIC_KEY="$test_dir/key.pub"
export REPO_DIR="$test_dir/repo" REPO_ID=test GENERATION=1 TTL=3600
mkdir -p "$test_dir/payload/bin"
package first 1.0.0-r1 "$test_dir/first.spkg"
make --no-print-directory -C "$ROOT" publish \
	REPO_DIR="$REPO_DIR" REPO_ID=test GENERATION=1 TTL=3600 \
	SIGNING_PUBLIC_KEY="$SIGNING_PUBLIC_KEY" PACKAGE_FILES="$test_dir/first.spkg"
index="$REPO_DIR/snapshots/1/index.json"
signature="$REPO_DIR/snapshots/1/index.sig"
"$REPOSITORY_TOOL" verify "$SIGNING_PUBLIC_KEY" "$index" "$signature"
jq -e '.format == 2 and .repo == "test" and .packages[0].manifest.description == "Проверка подписи"' \
	"$index" >/dev/null

# OpenSSL проверяет сообщение независимо от реализации протокола SSH-agent
{
	printf '%b' '\x30\x2a\x30\x05\x06\x03\x2b\x65\x70\x03\x21\x00'
	"$REPOSITORY_TOOL" key "$SIGNING_PUBLIC_KEY" | tr -d '\n' | unhex
} >"$test_dir/key.der"
{
	printf 'SynapseOS repository index v2\0'
	sha256sum "$index" | cut -d' ' -f1 | tr -d '\n' | unhex
} >"$test_dir/message"
unhex <"$signature" >"$test_dir/signature"
openssl pkeyutl -verify -pubin -keyform DER -inkey "$test_dir/key.der" \
	-rawin -in "$test_dir/message" -sigfile "$test_dir/signature"

cp "$index" "$test_dir/tampered.json"
printf ' ' >>"$test_dir/tampered.json"
reject "$REPOSITORY_TOOL" verify "$SIGNING_PUBLIC_KEY" "$test_dir/tampered.json" "$signature"
reject bash "$ROOT/build/repository.sh" "$test_dir/first.spkg"
export GENERATION=2
reject env REPO_ID=official bash "$ROOT/build/repository.sh" "$test_dir/first.spkg"
reject env SSH_AUTH_SOCK= bash "$ROOT/build/repository.sh" "$test_dir/first.spkg"
reject bash "$ROOT/build/repository.sh" "$test_dir/first.spkg" "$test_dir/first.spkg"
package changed 1.0.0-r1 "$test_dir/changed.spkg"
reject bash "$ROOT/build/repository.sh" "$test_dir/changed.spkg"

cp "$test_dir/first.spkg" "$test_dir/corrupt.spkg"
printf X | dd of="$test_dir/corrupt.spkg" bs=1 seek=1024 conv=notrunc status=none
reject bash "$ROOT/build/repository.sh" "$test_dir/corrupt.spkg"

ln -s /etc/passwd "$test_dir/payload/bin/link"
tar --format=ustar --mtime=@0 --owner=0 --group=0 --numeric-owner \
	-C "$test_dir" -cf "$test_dir/link.spkg" manifest.json payload/bin/link
reject bash "$ROOT/build/repository.sh" "$test_dir/link.spkg"

jq -e '.generation == 1' "$REPO_DIR/current.json" >/dev/null
[[ ! -e $REPO_DIR/snapshots/2 ]]
package second 1.0.0-r2 "$test_dir/second.spkg"
mkdir "$test_dir/out"
cp "$test_dir/first.spkg" "$test_dir/second.spkg" "$test_dir/out/"
cp "$test_dir/manifest.json" "$test_dir/out/signing-test.manifest.json"
make --no-print-directory -C "$ROOT" publish \
	REPO_DIR="$REPO_DIR" REPO_ID=test GENERATION=2 TTL=3600 OUT="$test_dir/out" \
	SIGNING_PUBLIC_KEY="$SIGNING_PUBLIC_KEY" PACKAGE_FILES=
jq -e '.generation == 2' "$REPO_DIR/current.json" >/dev/null
jq -e '.packages | length == 1 and .[0].manifest.version == "1.0.0-r2"' \
	"$REPO_DIR/snapshots/2/index.json" >/dev/null
"$REPOSITORY_TOOL" verify "$SIGNING_PUBLIC_KEY" \
	"$REPO_DIR/snapshots/2/index.json" "$REPO_DIR/snapshots/2/index.sig"

if [[ -n ${REPOSITORY_TEST_OUT:-} ]]; then
	make --no-print-directory -C "$ROOT" publish \
		REPO_DIR="$REPO_DIR" REPO_ID=test GENERATION=3 TTL=3600 OUT="$REPOSITORY_TEST_OUT" \
		SIGNING_PUBLIC_KEY="$SIGNING_PUBLIC_KEY" PACKAGE_FILES=
	jq '{format,repo,generation,packages:(.packages|length)}' "$REPO_DIR/snapshots/3/index.json"
fi

printf 'REPOSITORY_SIGNING_TEST_PASS\n'
