#!/bin/bash
# cxx-shared.sh: C++ для общих библиотек musl и Mesa
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: графические пакеты
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

set -euo pipefail

musl="$SDK/apps/musl"
headers="$BUILD_ROOT/mesa/cxx-headers/usr/include/c++/14.2.0"
flags=(-nostdinc -nostdinc++ -isystem "$headers" -isystem "$headers/x86_64-alpine-linux-musl"
	-isystem "$headers/backward" -isystem "$(gcc -print-file-name=include)"
	-isystem "$musl/obj/include" -isystem "$musl/include" -isystem "$musl/arch/x86_64"
	-isystem "$musl/arch/generic" -fno-stack-protector -fPIC -mno-red-zone)
mode=${SYNAPSE_LINK_MODE:-dynamic}
link=1

for arg; do
	case "$arg" in
		-c | -E | -S | -M | -MM | --version | -dump*)
			link=0
			;;
		-shared)
			mode=shared
			;;
	esac
done

if [[ $link == 0 ]]; then
	exec g++ "${flags[@]}" "$@"
fi

linkflags=(-shared)

if [[ $mode != shared ]]; then
	linkflags=(-pie -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 "$musl/lib/Scrt1.o")
fi

exec g++ "${flags[@]}" -nostdlib "${linkflags[@]}" "$musl/lib/crti.o" \
	"$(gcc -print-file-name=crtbeginS.o)" "$@" -L"$musl/lib" \
	-L"$BUILD_ROOT/openjdk/source/usr/lib" -lstdc++ -lc -lgcc_s \
	"$(gcc -print-file-name=crtendS.o)" "$musl/lib/crtn.o"
