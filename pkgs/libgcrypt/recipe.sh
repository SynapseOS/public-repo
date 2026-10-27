# recipe.sh: собрать Libgcrypt для проверки подписей Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 26.10.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d /tmp/synapse-libgcrypt-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib" "$sysroot/usr/bin"

stage="$BUILD_ROOT/libgpg-error/payload"
cp "$stage/usr/include/gpg-error.h" "$sysroot/usr/include/"
cp "$stage/usr/lib/libgpg-error.a" "$sysroot/usr/lib/"
sed -e "s|^prefix=/usr$|prefix=$sysroot/usr|" \
	-e "s|^libdir=/usr/lib$|libdir=$sysroot/usr/lib|" \
	"$stage/bin/gpg-error-config" >"$sysroot/usr/bin/gpg-error-config"
chmod +x "$sysroot/usr/bin/gpg-error-config"

mkdir -p "$work/build"
(
	cd "$work/build"
	PATH="$sysroot/usr/bin:$PATH" CC="$CC" \
		CFLAGS="-O2 -fPIC -I$sysroot/usr/include" \
		LDFLAGS="-L$sysroot/usr/lib" \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static \
			--with-libgpg-error-prefix="$sysroot/usr" --disable-doc
	make -j"$JOBS"
)

copy "$work/build/src/.libs/libgcrypt.a" usr/lib/libgcrypt.a
copy "$work/build/src/gcrypt.h" usr/include/gcrypt.h
copy "$work/build/src/libgcrypt-config" bin/libgcrypt-config
copy "$source/COPYING" usr/share/licenses/libgcrypt/COPYING
