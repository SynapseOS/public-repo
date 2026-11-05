# recipe.sh: собрать libassuan для GPGME
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: связь проверки подписей с сервисом GnuPG
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-libassuan-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig" "$sysroot/usr/bin"
cp "$BUILD_ROOT/libgpg-error/payload/usr/include/gpg-error.h" "$sysroot/usr/include/"
cp "$BUILD_ROOT/libgpg-error/payload/usr/lib/libgpg-error.a" "$sysroot/usr/lib/"
cp "$BUILD_ROOT/libgpg-error/payload/usr/lib/pkgconfig/gpg-error.pc" "$sysroot/usr/lib/pkgconfig/"
sed -e "s|^prefix=/usr$|prefix=$sysroot/usr|" \
	-e "s|^libdir=/usr/lib$|libdir=$sysroot/usr/lib|" \
	"$BUILD_ROOT/libgpg-error/payload/bin/gpg-error-config" >"$sysroot/usr/bin/gpg-error-config"
chmod +x "$sysroot/usr/bin/gpg-error-config"

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS="-O2 -fPIC -I$sysroot/usr/include" \
		LDFLAGS="-L$sysroot/usr/lib" \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static \
			--with-libgpg-error-prefix="$sysroot/usr" --disable-doc
	make -j"$JOBS"
)

copy "$work/build/src/.libs/libassuan.a" usr/lib/libassuan.a
copy "$work/build/src/assuan.h" usr/include/assuan.h
copy "$work/build/src/libassuan-config" bin/libassuan-config
copy "$source/COPYING" usr/share/licenses/libassuan/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libassuan.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libassuan
Description: GnuPG IPC library
Version: 3.0.2
Requires: gpg-error
Libs: -L${libdir} -lassuan
Cflags: -I${includedir}
PC
