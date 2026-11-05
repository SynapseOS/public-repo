# recipe.sh: собрать YAML-разборщик для AppStream
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека чтения метаданных приложений
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"
sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-fyaml-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$sysroot"
ln -s "$sysroot/usr/include/x86_64-linux-gnu/asm" "$sysroot/usr/include/asm"

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS="-O2 -fPIC -isystem $(gcc -print-file-name=include) -I$sysroot/usr/include" \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static
	make -j"$JOBS" -C src libfyaml.la
)

copy "$work/build/src/.libs/libfyaml.a" usr/lib/libfyaml.a

for header in "$source/include/"*.h "$source/include/libfyaml/"*.h; do
	if [[ $header == "$source/include/libfyaml/"* ]]; then
		copy "$header" "usr/include/libfyaml/$(basename "$header")"
	else
		copy "$header" "usr/include/$(basename "$header")"
	fi
done

copy "$source/LICENSE" usr/share/licenses/libfyaml/LICENSE

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libfyaml.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libfyaml
Description: YAML parser and emitter
Version: 0.9.6
Libs: -L${libdir} -lfyaml -lm
Cflags: -I${includedir}
PC
