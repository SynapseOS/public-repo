# recipe.sh: собрать dpkg для отдельного Debian-окружения
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-debian-dpkg-sysroot.XXXXXX")
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
		--prefix=/usr --bindir=/bin --libdir=/usr/lib --datadir=/usr/share/debian-apt \
		--with-admindir=/var/lib/dpkg \
		--disable-nls --disable-dselect --disable-start-stop-daemon \
		--disable-update-alternatives --disable-devel-docs \
		--with-libz --with-liblzma --without-libbz2 \
		--without-libzstd --without-libselinux

	make -C lib -j"$JOBS"
	make -C src -j"$JOBS" dpkg dpkg-split dpkg-query dpkg-divert \
		dpkg-statoverride dpkg-trigger
)

copy "$work/build/src/dpkg" usr/libexec/debian-apt/dpkg-native
copy "$work/build/src/dpkg-divert" usr/libexec/debian-apt/dpkg-divert
copy "$work/build/src/dpkg-statoverride" usr/libexec/debian-apt/dpkg-statoverride
copy "$work/build/src/dpkg-trigger" usr/libexec/debian-apt/dpkg-trigger
copy "$work/build/src/dpkg-split" bin/dpkg-split
copy "$work/build/src/dpkg-query" bin/dpkg-query

for table in cputable ostable tupletable; do
	copy "$source/data/$table" "usr/share/debian-apt/dpkg/$table"
done

copy "$source/COPYING" usr/share/licenses/debian-dpkg/COPYING
