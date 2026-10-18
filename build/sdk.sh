#!/bin/bash
# sdk.sh: подготовка открытого SDK и libc SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный
set -euo pipefail
mkdir -p "$SDK"
exec 9>"$SDK/build.lock"
flock 9
cp -au "$ROOT/sdk/." "$SDK/"

for directory in apps include assets; do
	if [[ ! -e $ROOT/$directory ]]; then
		ln -s "$SDK/$directory" "$ROOT/$directory"
	fi
done

if [[ ! -f $SDK/apps/musl/config.mak ]]; then
	(
		cd "$SDK/apps/musl" &&
			./configure --target=x86_64-synapse --enable-shared --enable-static \
				CC=gcc AR=ar RANLIB=ranlib CROSS_COMPILE='' CFLAGS='-Os -pipe -fno-stack-protector'
	)
fi

make -C "$SDK/apps/musl" -j"$JOBS"
