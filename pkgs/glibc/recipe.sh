# recipe.sh: собрать отдельный динамический runtime glibc для Linux ABI SynapseOS
# Автор: Елчинян Арен Артурович
# Год: 2026
# Архитектура: x86_64
# ABI: Linux x86_64
#
# Описание: сборка и упаковка отдельных динамических библиотек glibc
# Подсистема: сборка пакетов
# Первое изменение: 21.10.2026
# Последнее изменение: 21.10.2026
# Статус доступа: общедоступный

prepare

build_dir="$work/build"
install_dir="$work/install"
headers_dir="$work/headers"
rm -rf -- "$build_dir" "$install_dir" "$headers_dir"
mkdir -p "$build_dir" "$install_dir" "$headers_dir"
ln -s /usr/include/x86_64-linux-gnu/asm "$headers_dir/asm"
ln -s /usr/include/asm-generic "$headers_dir/asm-generic"
ln -s /usr/include/linux "$headers_dir/linux"

(
	cd "$build_dir"
	libc_cv_slibdir=/lib libc_cv_rtlddir=/lib CC=gcc \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-gnu \
			--prefix=/usr --libdir=/usr/lib --with-headers="$headers_dir" \
			--disable-werror --disable-build-nscd --disable-nscd
)

make -C "$build_dir" -j"$JOBS"
make -C "$build_dir" install "install_root=$install_dir"

for library in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libresolv.so.2 \
	libnss_files.so.2 libnss_dns.so.2; do
	if [[ ! -f "$install_dir/lib/$library" ]]; then
		echo "Missing glibc runtime library: $library" >&2
		exit 1
	fi

	strip --strip-unneeded "$install_dir/lib/$library"
	copy "$install_dir/lib/$library" "lib/$library"
done

for library in libmvec.so.1 libpthread.so.0 libdl.so.2 librt.so.1 \
	libutil.so.1 libanl.so.1 libBrokenLocale.so.1; do
	if [[ -f "$install_dir/lib/$library" ]]; then
		strip --strip-unneeded "$install_dir/lib/$library"
		copy "$install_dir/lib/$library" "lib/$library"
	fi
done

copy "$source/COPYING.LIB" usr/share/licenses/glibc/COPYING.LIB
copy "$source/LICENSES" usr/share/licenses/glibc/LICENSES
