#!/bin/bash
# check-signature.sh: проверить подлинный и изменённый индекс Debian
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: проверка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
keyring="$root/pkgs/.work/debian-archive-keyring/payload/usr/share/keyrings/debian-archive-keyring.pgp"

if [[ ! -f $keyring ]]; then
	echo 'Debian keyring package has not been built' >&2
	exit 1
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/synapse-apt-signature.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT

curl --fail --location --retry 3 \
	--output "$scratch/InRelease" \
	https://deb.debian.org/debian/dists/trixie/InRelease

gpgv --keyring "$keyring" "$scratch/InRelease"
sed 's/^Suite: /Suite: forged-/' "$scratch/InRelease" > "$scratch/InRelease-changed"

if ! grep -q '^Suite: forged-' "$scratch/InRelease-changed"; then
	echo 'Could not alter the release metadata' >&2
	exit 1
fi

if gpgv --keyring "$keyring" "$scratch/InRelease-changed" >"$scratch/invalid.log" 2>&1; then
	echo 'Changed release metadata was accepted' >&2
	exit 1
fi

echo 'Signed Debian index accepted and altered index rejected'
