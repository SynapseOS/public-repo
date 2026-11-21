#!/bin/bash
# sdk.sh: подготовка открытого SDK и libc SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный
set -euo pipefail
mkdir -p "$SDK"
exec 9>"$SDK/build.lock"
flock 9
cp -a "$ROOT/sdk/." "$SDK/"

for directory in apps include assets; do
	if [[ $TARGET_ARCH == x86_64 && ! -e $ROOT/$directory ]]; then
		ln -s "$SDK/$directory" "$ROOT/$directory"
	fi
done

cd "$SDK/apps/musl"
if [[ -f config.mak ]] && ! grep -Fxq "ARCH = $TARGET_ARCH" config.mak; then
	echo "SDK contains a different architecture: $SDK" >&2
	exit 1
fi

if [[ $TARGET_ARCH == aarch64 ]]; then
	: "${ARM64_CC:=clang}" "${ARM64_LIBGCC:=$(aarch64-linux-gnu-gcc -print-libgcc-file-name)}"
	test -f "$ARM64_LIBGCC"
	if [[ ! -f config.mak ]]; then
		CC="$ARM64_CC --target=aarch64-linux-musl -fuse-ld=lld -Wno-unused-command-line-argument" \
			AR=llvm-ar RANLIB=llvm-ranlib LIBCC="$ARM64_LIBGCC" \
			sh ./configure --target=aarch64-linux-musl --disable-wrapper
	fi
	make -j"$JOBS" ARCH=aarch64 LIBCC="$ARM64_LIBGCC" \
		'BASE_SRCS=$(sort $(filter-out $(srcdir)/src/synapse/%,$(wildcard $(BASE_GLOBS))))'
	exit
fi

if [[ ! -f $SDK/apps/musl/config.mak ]]; then
	(
		cd "$SDK/apps/musl" &&
			./configure --target=x86_64-synapse --enable-shared --enable-static \
				CC=gcc AR=ar RANLIB=ranlib CROSS_COMPILE='' CFLAGS='-Os -pipe -fno-stack-protector'
	)
fi

make -C "$SDK/apps/musl" -j"$JOBS"
