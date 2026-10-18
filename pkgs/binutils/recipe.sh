# recipe.sh: Собрать нативные binutils с целевой схемой ELF и TLS
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
source "$ROOT/build/toolchain.sh"
rm -rf "$work/build"
mkdir -p "$work/build"

(
	cd "$work/build" &&
		"$source/configure" --build=x86_64-pc-linux-gnu --host=x86_64-linux-musl \
			--target=x86_64-linux-musl --prefix=/usr --disable-nls --disable-shared --enable-static \
			--disable-werror --disable-gprofng --disable-gdb --disable-gdbserver --disable-sim \
			--disable-debuginfod --without-zstd --without-system-zlib --disable-plugins
)

make -C "$work/build" -j"$JOBS"

for pair in as:gas/as-new ld:ld/ld-new ar:binutils/ar ranlib:binutils/ranlib nm:binutils/nm-new \
	objcopy:binutils/objcopy objdump:binutils/objdump readelf:binutils/readelf strip:binutils/strip-new \
	size:binutils/size gstrings:binutils/strings addr2line:binutils/addr2line c++filt:binutils/cxxfilt; do
	copy "$work/build/${pair#*:}" "bin/${pair%%:*}"
	strip --strip-debug "$payload/bin/${pair%%:*}"
done

copy "$source/COPYING3" usr/share/licenses/binutils/COPYING3
