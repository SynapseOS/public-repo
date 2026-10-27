# recipe.sh: упаковать glibc для разработки в отдельный sysroot
# Автор: Елчинян Арен Артурович
# Год: 2026
# Архитектура: x86_64
# ABI: Linux x86_64
#
# Описание: изолированная копия заголовков и файлов для линковки glibc
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026
# Последнее изменение: 25.10.2026
# Статус доступа: общедоступный

install_dir="$BUILD_ROOT/glibc/install"
sysroot="usr/share/glibc/sysroot"

runtime_version=$(jq -er .version "$OUT/glibc.manifest.json")
dev_version=$(jq -er .version "$port/manifest.json")

if [[ $runtime_version != $dev_version ]]; then
	echo "glibc-dev $dev_version requires glibc $dev_version build output" >&2
	exit 1
fi

for required in usr/include/stdio.h usr/lib/crt1.o usr/lib/crti.o usr/lib/crtn.o usr/lib/libc.so \
	lib/libc.so.6 lib/ld-linux-x86-64.so.2; do
	if [[ ! -f "$install_dir/$required" ]]; then
		echo "Missing glibc development input: $required" >&2
		exit 1
	fi
done

for required in linux/types.h asm-generic/errno.h x86_64-linux-gnu/asm/unistd.h; do
	if [[ ! -f "$BUILD_ROOT/glibc/host-tools/root/usr/include/$required" ]]; then
		echo "Missing pinned Linux UAPI header: $required" >&2
		exit 1
	fi
done

while IFS= read -r -d '' file; do
	copy "$file" "$sysroot/${file#"$install_dir/"}"
done < <(find "$install_dir/usr/include" \( -type f -o -type l \) -print0 | sort -z)

for directory in linux asm-generic; do
	while IFS= read -r -d '' file; do
		copy "$file" "$sysroot/usr/include/$directory/${file#"$BUILD_ROOT/glibc/host-tools/root/usr/include/$directory/"}"
	done < <(find "$BUILD_ROOT/glibc/host-tools/root/usr/include/$directory" \
		\( -type f -o -type l \) -print0 | sort -z)
done

while IFS= read -r -d '' file; do
	copy "$file" "$sysroot/usr/include/asm/${file#"$BUILD_ROOT/glibc/host-tools/root/usr/include/x86_64-linux-gnu/asm/"}"
done < <(find "$BUILD_ROOT/glibc/host-tools/root/usr/include/x86_64-linux-gnu/asm" \
	\( -type f -o -type l \) -print0 | sort -z)

for directory in usr/lib lib; do
	while IFS= read -r -d '' file; do
		copy "$file" "$sysroot/${file#"$install_dir/"}"
	done < <(find "$install_dir/$directory" -maxdepth 1 \( -type f -o -type l \) -print0 | sort -z)
done

copy "$BUILD_ROOT/glibc/glibc-045c51476c7abf6144815a4db44f51a118b4cb3e/COPYING.LIB" \
	usr/share/licenses/glibc-dev/COPYING.LIB
