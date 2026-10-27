# recipe.sh: собрать статическую проверку и восстановление ext
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 27.10.2026, последнее изменение: 27.10.2026
# Статус доступа: общедоступный

prepare

build="$work/build"
mkdir -p "$build" "$work/include/linux"

# На x86_64 e2fsprogs вызывает lseek и не использует старый номер llseek
printf '%s\n' '// Старые номера llseek не нужны при 64-битном off_t' > "$work/include/linux/unistd.h"

export CFLAGS='-O2 -fno-strict-aliasing'
export PKG_CONFIG=false

(
	cd "$build"
	"$source/configure" --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
		--disable-nls --disable-uuidd --disable-fuse2fs --disable-fsck \
		--enable-libuuid --enable-libblkid --prefix=/usr
)

cppflags="-I$work/include -I. -I$build/lib -I$source/lib"
make -C "$build" -j"$JOBS" libs CPPFLAGS="$cppflags"
make -C "$build/e2fsck" -j"$JOBS" CPPFLAGS="$cppflags"

copy "$build/e2fsck/e2fsck" bin/e2fsck
copy "$source/NOTICE" usr/share/licenses/e2fsprogs/NOTICE
