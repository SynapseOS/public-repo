# recipe.sh: собрать dpkg-deb без установки Debian-пакетов в корень системы
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-dpkg-deb-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib"

for package in zlib liblzma libmd; do
	stage="$BUILD_ROOT/$package/payload"

	if [[ ! -d $stage/usr/include || ! -d $stage/usr/lib ]]; then
		echo "Missing $package build dependency" >&2
		exit 1
	fi

	cp -a "$stage/usr/include/." "$sysroot/usr/include/"
	cp -a "$stage/usr/lib/." "$sysroot/usr/lib/"
done

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS='-O2 -D__synapse__' \
		CPPFLAGS="-I$sysroot/usr/include" LDFLAGS="-static -L$sysroot/usr/lib" \
		PKG_CONFIG=false "$source/configure" \
		--build=x86_64-linux-gnu --host=x86_64-linux-musl \
		--prefix=/usr --bindir=/bin --libdir=/usr/lib \
		--disable-nls --disable-dselect --disable-start-stop-daemon \
		--disable-update-alternatives --disable-devel-docs \
		--with-libz --with-liblzma --without-libbz2 \
		--without-libzstd --without-libselinux
	sed -i 's|^#define TAR "tar"$|#define TAR "/bin/gtar"|' config.h

	make -C lib -j"$JOBS"
	make -C src -j"$JOBS" dpkg-deb
)

copy "$work/build/src/dpkg-deb" bin/dpkg-deb
copy "$source/COPYING" usr/share/licenses/dpkg-deb/COPYING
