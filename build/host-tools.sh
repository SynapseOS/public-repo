#!/bin/bash
# host-tools.sh: локальные закреплённые инструменты разбора исходников
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

set -euo pipefail
source "$ROOT/build/common.sh"

if [[ -x $BUILD_ROOT/host-tools/root/usr/bin/bison ]]; then
	exit 0
fi

command -v dpkg-deb >/dev/null
mkdir -p "$BUILD_ROOT/host-tools"
exec 9>"$BUILD_ROOT/host-tools/build.lock"
flock 9

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$BUILD_ROOT/host-tools/$filename"
	dpkg-deb -x "$BUILD_ROOT/host-tools/$filename" "$BUILD_ROOT/host-tools/root"
done < <(jq -r '.[] | [.file,.source,.sha256] | @tsv' "$ROOT/pkgs/libpcap/host-tools.json")
