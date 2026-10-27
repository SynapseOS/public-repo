# recipe.sh: собрать интерфейс проверки подписей Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека GPGME без встроенных ключей
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d /tmp/synapse-gpgme-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig" "$sysroot/usr/bin"

for package in libgpg-error libassuan; do
	stage="$BUILD_ROOT/$package/payload"
	cp -a "$stage/usr/include/." "$sysroot/usr/include/"
	cp -a "$stage/usr/lib/." "$sysroot/usr/lib/"
done

for config in gpg-error-config libassuan-config; do
	package=libgpg-error
	if [[ $config == libassuan-config ]]; then
		package=libassuan
	fi

	sed -e "s|^prefix=/usr$|prefix=$sysroot/usr|" \
		-e "s|^libdir=/usr/lib$|libdir=$sysroot/usr/lib|" \
		"$BUILD_ROOT/$package/payload/bin/$config" >"$sysroot/usr/bin/$config"
	chmod +x "$sysroot/usr/bin/$config"
done

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS="-O2 -fPIC -I$sysroot/usr/include" \
		LDFLAGS="-L$sysroot/usr/lib" \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --disable-shared --enable-static \
			--with-libgpg-error-prefix="$sysroot/usr" \
			--with-libassuan-prefix="$sysroot/usr" \
			--disable-gpg-test --disable-gpgsm-test --disable-gpgconf-test
	make -j"$JOBS"
)

copy "$work/build/src/.libs/libgpgme.a" usr/lib/libgpgme.a
copy "$work/build/src/gpgme.h" usr/include/gpgme.h
copy "$source/COPYING" usr/share/licenses/gpgme/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/gpgme.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: gpgme
Description: GnuPG Made Easy library
Version: 2.2.0
Requires: libassuan gpg-error
Libs: -L${libdir} -lgpgme
Cflags: -I${includedir}
PC
