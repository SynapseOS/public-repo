#!/bin/bash
# qmi-stack.sh: ARM64 сборка стека модема с существующим musl sysroot
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: ARM64, ABI: synapse-aarch64-v1
# Подсистема: сборка пакетов
# Первое изменение: 25.11.2026, последнее изменение: 25.11.2026
# Статус доступа: общедоступный
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
artifact_root=$(realpath "${QMI_WORK_ROOT:-$ROOT/../work/pixel3a-port}")
stack=${QMI_STACK_ROOT:-"$artifact_root/qmi-stack"}
export QMI_SYSROOT=${QMI_SYSROOT:-"$artifact_root/qt-sdk/root"}
export QMI_GCC_DIR=${QMI_GCC_DIR:-"$QMI_SYSROOT/usr/lib/gcc/aarch64-alpine-linux-musl/14.2.0"}
export QMI_LLD=${QMI_LLD:-"$artifact_root/tools/root/usr/lib/llvm-19/bin/ld.lld"}
export QMI_STRIP=${QMI_STRIP:-"$artifact_root/tools/root/usr/bin/llvm-strip-19"}
export QMI_CLANG=${QMI_CLANG:-/usr/bin/clang}
export QMI_EXTRA_HEADERS="$stack/sdk/include"
base_sdk=${QMI_BASE_SDK:-"$ROOT/.sdk"}
[[ $(df -Pk "$ROOT" | awk 'END {print $4}') -ge 20971520 ]] || {
	echo 'At least 20 GiB free disk space is required' >&2
	exit 1
}
for resource in "$QMI_SYSROOT/usr/lib/libc.a" "$QMI_GCC_DIR" "$QMI_LLD" "$QMI_STRIP" \
	"$base_sdk/apps/musl/COPYRIGHT" "$base_sdk/apps/musl/include/libintl.h"; do
	test -e "$resource"
done
export ROOT TARGET_ARCH=aarch64 JOBS=${JOBS:-8}
export BUILD_ROOT="$stack/build" OUT="$stack/out" SDK="$stack/sdk"
mkdir -p "$SDK/apps/apps/common" "$SDK/apps/musl" "$QMI_EXTRA_HEADERS" "$stack/tools"
cp "$base_sdk/apps/musl/COPYRIGHT" "$SDK/apps/musl/COPYRIGHT"
cp "$base_sdk/apps/musl/include/libintl.h" "$QMI_EXTRA_HEADERS/libintl.h"
cp "$ROOT/build/qmi-cross-cc" "$SDK/apps/apps/common/musl-cc"
chmod +x "$SDK/apps/apps/common/musl-cc"
# GNU tar поддерживает используемые prepare аргументы; полная установка SDK не нужна.
if ! command -v bsdtar >/dev/null; then
	ln -sf "$(command -v tar)" "$stack/tools/bsdtar"
fi
export PATH="$stack/tools:$PATH"
for package in ${*:-libffi pcre2 zlib glib glib-gio qrtr-glib libqmi}; do
	case "$package" in
		libffi|pcre2|zlib|glib|glib-gio|qrtr-glib|libqmi) ;;
		*) echo "Unsupported QMI stack package: $package" >&2; exit 1 ;;
	esac
	mkdir -p "$BUILD_ROOT/$package"
	if [[ ! -e $BUILD_ROOT/$package/source.tar && -f $ROOT/pkgs/.work/$package/source.tar ]]; then
		cp --reflink=auto "$ROOT/pkgs/.work/$package/source.tar" "$BUILD_ROOT/$package/source.tar"
	fi
	bash "$ROOT/build/package.sh" "$package"
done
