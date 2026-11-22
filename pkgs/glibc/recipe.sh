# recipe.sh: собрать динамический runtime glibc для Linux ABI SynapseOS
# Автор: Елчинян Арен Артурович
# Год: 2026
# Архитектура: x86_64
# ABI: Linux x86_64
#
# Описание: сборка и упаковка библиотек glibc, NSS и преобразования кодировок
# Подсистема: сборка пакетов
# Первое изменение: 21.10.2026
# Последнее изменение: 25.10.2026
# Статус доступа: общедоступный

host_tools="$work/host-tools"
mkdir -p "$host_tools/root"

if ! command -v dpkg-deb >/dev/null; then
	echo 'Missing host tool: dpkg-deb' >&2
	exit 1
fi

if ! command -v gzip >/dev/null; then
	echo 'Missing host tool: gzip' >&2
	exit 1
fi

while IFS=$'\t' read -r filename url digest; do
	fetch "$url" "$digest" "$host_tools/$filename"
	dpkg-deb -x "$host_tools/$filename" "$host_tools/root"
done < <(jq -r '.[] | [.file,.source,.sha256] | @tsv' "$port/host-tools.json")

for header in linux/types.h asm-generic/errno.h x86_64-linux-gnu/asm/unistd.h; do
	if [[ ! -f "$host_tools/root/usr/include/$header" ]]; then
		echo "Missing pinned Linux UAPI header: $header" >&2
		exit 1
	fi
done

export PATH="$host_tools/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="$host_tools/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

prepare

build_dir="$work/build"
install_dir="$work/install"
headers_dir="$work/headers"
rm -rf -- "$build_dir" "$install_dir" "$headers_dir"
mkdir -p "$build_dir" "$install_dir" "$headers_dir"
ln -s "$host_tools/root/usr/include/x86_64-linux-gnu/asm" "$headers_dir/asm"
ln -s "$host_tools/root/usr/include/asm-generic" "$headers_dir/asm-generic"
ln -s "$host_tools/root/usr/include/linux" "$headers_dir/linux"

(
	cd "$build_dir"
	libc_cv_slibdir=/lib libc_cv_rtlddir=/lib CC=gcc \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-gnu \
			--prefix=/usr --libdir=/usr/lib --with-headers="$headers_dir" \
			--disable-werror --disable-build-nscd --disable-nscd --without-selinux
)

env -u ARCH -u TARGET_ARCH make -C "$build_dir" -j"$JOBS"
env -u ARCH -u TARGET_ARCH make -C "$build_dir" install "install_root=$install_dir"

for library in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libresolv.so.2 \
	libnss_files.so.2 libnss_dns.so.2; do
	if [[ ! -f "$install_dir/lib/$library" ]]; then
		echo "Missing glibc runtime library: $library" >&2
		exit 1
	fi
done

copy "$install_dir/lib/ld-linux-x86-64.so.2" lib/ld-linux-x86-64.so.2
strip --strip-unneeded "$payload/lib/ld-linux-x86-64.so.2"

for library in "$install_dir"/lib/lib*.so*; do
	if [[ -f "$library" ]]; then
		copy "$library" "lib/${library##*/}"
		strip --strip-unneeded "$payload/lib/${library##*/}"
	fi
done

while IFS= read -r -d '' file; do
	relative=${file#"$install_dir/"}
	copy "$file" "$relative"

	if [[ $file == *.so ]]; then
		strip --strip-unneeded "$payload/$relative"
	fi
done < <(find "$install_dir/usr/lib/gconv" -type f -print0 | sort -z)

for utility in locale localedef; do
	if [[ ! -f "$install_dir/usr/bin/$utility" ]]; then
		echo "Missing glibc utility: $utility" >&2
		exit 1
	fi

	copy "$install_dir/usr/bin/$utility" "bin/$utility"
	strip --strip-unneeded "$payload/bin/$utility"
done

if [[ ! -f "$install_dir/usr/bin/getconf" ]]; then
	echo 'Missing glibc utility: getconf' >&2
	exit 1
fi

copy "$install_dir/usr/bin/getconf" usr/libexec/glibc/getconf
strip --strip-unneeded "$payload/usr/libexec/glibc/getconf"

while IFS= read -r -d '' file; do
	relative=${file#"$install_dir/"}

	if [[ $file == "$install_dir/usr/share/i18n/charmaps/"*.gz ]]; then
		relative=${relative%.gz}
		mkdir -p "$(dirname "$payload/$relative")"
		gzip -cd -- "$file" > "$payload/$relative"
	else
		copy "$file" "$relative"
	fi
done < <(find "$install_dir/usr/share/i18n" -type f -print0 | sort -z)

locale_root="$work/locale-data"
rm -rf -- "$locale_root"
mkdir -p "$locale_root/usr/lib/locale"

for locale_name in en_US ru_RU; do
	I18NPATH="$install_dir/usr/share/i18n" \
		"$install_dir/lib/ld-linux-x86-64.so.2" \
		--library-path "$install_dir/lib" \
		"$install_dir/usr/bin/localedef" --no-archive --no-hard-links \
		--prefix="$locale_root" -i "$locale_name" -f UTF-8 "$locale_name.UTF-8"

	if [[ ! -f "$locale_root/usr/lib/locale/$locale_name.utf8/LC_CTYPE" ]]; then
		echo "Missing compiled locale: $locale_name.UTF-8" >&2
		exit 1
	fi
done

while IFS= read -r -d '' file; do
	copy "$file" "${file#"$locale_root/"}"
done < <(find "$locale_root/usr/lib/locale" -type f -print0 | sort -z)

copy "$install_dir/usr/share/locale/locale.alias" usr/share/locale/locale.alias
copy "$source/COPYING.LIB" usr/share/licenses/glibc/COPYING.LIB
copy "$source/LICENSES" usr/share/licenses/glibc/LICENSES
