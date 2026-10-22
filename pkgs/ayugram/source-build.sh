#!/bin/bash
# source-build.sh: собрать AyuGram с локальными реквизитами приложения
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: сборка графических пакетов
# Первое изменение: 18.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный, MIT

set +x
set -euo pipefail
umask 077
source "$ROOT/build/common.sh"
port="$ROOT/pkgs/ayugram"
source_work="$BUILD_ROOT/ayugram-source"
api_file=${TELEGRAM_API_FILE:-$HOME/.config/synapseos/telegram-api.env}

# Передать вывод построчно, исключая локальные реквизиты даже при ошибке компилятора
redact_build_output() {
	local line

	while IFS= read -r line || [[ -n $line ]]; do
		line=${line//"$TDESKTOP_API_HASH"/'[API_HASH]'}
		line=${line//"$TDESKTOP_API_ID"/'[API_ID]'}
		printf '%s\n' "$line"
	done
}

if [[ -n ${TDESKTOP_API_ID:-} || -n ${TDESKTOP_API_HASH:-} ]]; then
	if [[ -z ${TDESKTOP_API_ID:-} || -z ${TDESKTOP_API_HASH:-} ]]; then
		printf 'Set both TDESKTOP_API_ID and TDESKTOP_API_HASH\n' >&2
		exit 1
	fi
else
	if [[ ! -f $api_file || -L $api_file || ! -O $api_file ]]; then
		printf 'Expected a local credential file owned by the current user\n' >&2
		exit 1
	fi

	mode=$(stat -c '%a' "$api_file")

	if (( (8#$mode & 077) != 0 )); then
		printf 'Credential file must have mode 600 or 400\n' >&2
		exit 1
	fi

	while IFS= read -r line || [[ -n $line ]]; do
		line=${line%$'\r'}

		if [[ -z $line || $line == \#* ]]; then
			continue
		fi

		if [[ ! $line =~ ^(TDESKTOP_API_ID|TDESKTOP_API_HASH)=(.*)$ ]]; then
			printf 'Invalid credential file syntax\n' >&2
			exit 1
		fi

		key=${BASH_REMATCH[1]}
		value=${BASH_REMATCH[2]}

		if [[ $value == \'*\' || $value == \"*\" ]]; then
			value=${value:1:${#value}-2}
		fi

		export "$key=$value"
	done <"$api_file"
fi

if [[ ! ${TDESKTOP_API_ID:-} =~ ^[1-9][0-9]{0,9}$ ||
	! ${TDESKTOP_API_HASH:-} =~ ^[a-fA-F0-9]{32}$ || ${TDESKTOP_API_ID:-} == 17349 ]]; then
	printf 'Provide your own numeric API ID and 32-character API hash\n' >&2
	exit 1
fi

if ((TDESKTOP_API_ID > 2147483647)); then
	printf 'API ID is outside the supported integer range\n' >&2
	exit 1
fi

if [[ ${1:-} == --check-credentials ]]; then
	printf 'AYUGRAM_CREDENTIALS_VALID\n'
	exit 0
fi

if [[ $# != 0 ]]; then
	printf 'Unsupported source build option\n' >&2
	exit 1
fi

mkdir -p "$source_work"
chmod 700 "$source_work"
exec 8>"$source_work/build.lock"
flock 8
fetch "$(jq -er .source.url "$port/port.json")" \
	"$(jq -er .source.sha256 "$port/port.json")" "$source_work/ayugram-7.2.9-full.tar.gz"
fetch "$(jq -er .build_root.url "$port/port.json")" \
	"$(jq -er .build_root.sha256 "$port/port.json")" "$source_work/alpine-minirootfs.tar.gz"
fetch "$(jq -er .tde2e.url "$port/port.json")" \
	"$(jq -er .tde2e.sha256 "$port/port.json")" "$source_work/tde2e.tar.gz"

if [[ ! -d $source_work/ayu-src ]]; then
	mkdir "$source_work/ayu-src"
	bsdtar -xf "$source_work/ayugram-7.2.9-full.tar.gz" -C "$source_work/ayu-src" --strip-components=1
fi

# Применить адаптации к закреплённым исходникам без повторного наложения
while IFS= read -r patch_file; do
	if patch --batch --forward --fuzz=0 --dry-run -p1 -d "$source_work/ayu-src" \
		-i "$port/$patch_file" >/dev/null 2>&1; then
		patch --batch --forward --fuzz=0 -p1 -d "$source_work/ayu-src" -i "$port/$patch_file"
	elif ! patch --batch --reverse --fuzz=0 --dry-run -p1 -d "$source_work/ayu-src" \
		-i "$port/$patch_file" >/dev/null 2>&1; then
		printf 'Source patch does not match the pinned source: %s\n' "$patch_file" >&2
		exit 1
	fi
done < <(jq -r '.patches // [] | .[]' "$port/port.json")

if [[ ! -d $source_work/tde2e-src ]]; then
	mkdir "$source_work/tde2e-src"
	bsdtar -xf "$source_work/tde2e.tar.gz" -C "$source_work/tde2e-src" --strip-components=1
fi

if [[ ! -d $source_work/root ]]; then
	mkdir "$source_work/root"
	bsdtar -xf "$source_work/alpine-minirootfs.tar.gz" -C "$source_work/root"
	sed -i 's,http://,https://,g' "$source_work/root/etc/apk/repositories"
	mkdir -p "$source_work/root/etc/ssl/certs"
	cp /etc/ssl/certs/ca-certificates.crt "$source_work/root/etc/ssl/certs/ca-certificates.crt"
fi

root_command=(bwrap --unshare-user --uid 1000 --gid 1000
	--bind "$source_work/root" / --dev /dev --proc /proc
	--ro-bind /etc/resolv.conf /etc/resolv.conf
	--unsetenv CC --unsetenv CXX --unsetenv AR --unsetenv LD
	--unsetenv CFLAGS --unsetenv CXXFLAGS --unsetenv CPPFLAGS --unsetenv LDFLAGS
	--unsetenv PKG_CONFIG --unsetenv PKG_CONFIG_PATH --unsetenv PKG_CONFIG_SYSROOT_DIR
	--unsetenv CMAKE_PREFIX_PATH --unsetenv CMAKE_TOOLCHAIN_FILE
	--unsetenv BISON_PKGDATADIR --unsetenv M4
	--unsetenv LD_PRELOAD --unsetenv LD_LIBRARY_PATH --unsetenv QT_PLUGIN_PATH
	--unsetenv XDG_CACHE_HOME --unsetenv XDG_RUNTIME_DIR
	--setenv PATH /usr/sbin:/usr/bin:/sbin:/bin
	--setenv QT_QPA_PLATFORM offscreen
	--setenv HOME /root)

lock_hash=$(sha256sum "$port/build-packages.lock" | cut -d ' ' -f 1)

if [[ ! -f $source_work/root/.build-ready ||
	$(cat "$source_work/root/.build-ready") != "$lock_hash" ]]; then
	mapfile -t packages <"$port/build-packages.lock"
	"${root_command[@]}" /sbin/apk update
	"${root_command[@]}" /sbin/apk add --no-progress "${packages[@]}"
	printf '%s\n' "$lock_hash" >"$source_work/root/.build-ready"
fi

"${root_command[@]}" --bind "$source_work" /work --ro-bind "$port" /port \
	/bin/sh /port/build-in-root.sh 2>&1 | tr '\r' '\n' | redact_build_output
