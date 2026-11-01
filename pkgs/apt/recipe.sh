# recipe.sh: собрать APT с отдельными каталогами данных Debian
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

glibc_stage="$BUILD_ROOT/glibc/payload"

for library in ld-linux-x86-64.so.2 libc.so.6; do
	if [[ ! -f $glibc_stage/lib/$library ]]; then
		echo "Missing Synapse glibc runtime library: $library" >&2
		exit 1
	fi
done

provider="$work/synapse-glibc-runtime"
rm -rf -- "$provider"
mkdir -p "$provider/DEBIAN" "$provider/lib" "$provider/lib64" \
	"$provider/usr/share/doc/synapse-glibc-runtime"
cp "$glibc_stage/lib/ld-linux-x86-64.so.2" "$provider/lib/"
cp "$glibc_stage/lib/libc.so.6" "$provider/lib/"
cp "$glibc_stage/usr/share/licenses/glibc/COPYING.LIB" \
	"$provider/usr/share/doc/synapse-glibc-runtime/"
cp "$glibc_stage/usr/share/licenses/glibc/LICENSES" \
	"$provider/usr/share/doc/synapse-glibc-runtime/"
ln -s ../lib/ld-linux-x86-64.so.2 "$provider/lib64/ld-linux-x86-64.so.2"

cat > "$provider/DEBIAN/control" <<'EOF'
Package: synapse-glibc-runtime
Version: 2.44.0-synapse1
Architecture: amd64
Maintainer: SynapseOS
Section: libs
Priority: optional
Provides: libc6 (= 2.44.0)
Description: Verified SynapseOS glibc runtime for isolated Debian packages
EOF

SOURCE_DATE_EPOCH=1761991200 dpkg-deb -Zxz --build --root-owner-group \
	"$provider" "$work/synapse-glibc-runtime.deb"
provider_sha=$(sha256sum "$work/synapse-glibc-runtime.deb")
provider_sha=${provider_sha%% *}
loader_sha=$(sha256sum "$glibc_stage/lib/ld-linux-x86-64.so.2")
loader_sha=${loader_sha%% *}
libc_sha=$(sha256sum "$glibc_stage/lib/libc.so.6")
libc_sha=${libc_sha%% *}

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-apt-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib"

for package in zlib liblzma libbz2 liblz4 libxxhash openssl; do
	stage="$BUILD_ROOT/$package/payload"

	if [[ ! -d $stage/usr/include || ! -d $stage/usr/lib ]]; then
		echo "Missing $package build dependency" >&2
		exit 1
	fi

	cp -a "$stage/usr/include/." "$sysroot/usr/include/"
	cp -a "$stage/usr/lib/." "$sysroot/usr/lib/"
done

triehash_archive="$work/triehash_0.3-3_all.deb"
curl --fail --location --retry 3 \
	--output "$triehash_archive" \
	https://deb.debian.org/debian/pool/main/t/triehash/triehash_0.3-3_all.deb
printf '%s  %s\n' \
	'83f4713563f3fec9b82587d5628da3840def9d4f7299c0131f687144c90fbff0' \
	"$triehash_archive" | sha256sum --check

mkdir -p "$work/triehash-root"
bsdtar -xf "$triehash_archive" -C "$work" data.tar.xz
bsdtar -xf "$work/data.tar.xz" -C "$work/triehash-root" ./usr/bin/triehash

cmake -E rm -rf "$work/build"
cmake -S "$source" -B "$work/build" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$ROOT/pkgs/libstdcxx/cxx" \
	-DCMAKE_C_FLAGS='-O2 -D__synapse__' \
	-DCMAKE_CXX_FLAGS="-O2 -D__synapse__ -I$sysroot/usr/include -Wno-unused-parameter -Wno-maybe-uninitialized" \
	-DCMAKE_EXE_LINKER_FLAGS="-static -L$sysroot/usr/lib" \
	-DCMAKE_FIND_ROOT_PATH="$sysroot" \
	-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
	-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
	-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
	-DICONV_INCLUDE_DIR="$ROOT/apps/musl/include" \
	-DTRIEHASH_EXECUTABLE="$work/triehash-root/usr/bin/triehash" \
	-DOPENSSL_ROOT_DIR="$sysroot/usr" \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCONF_DIR=/etc/debian-apt \
	-DSTATE_DIR=/var/lib/debian-apt \
	-DCACHE_DIR=/var/cache/debian-apt \
	-DLOG_DIR=/etc/debian-apt/log \
	-DLIBEXEC_DIR=/usr/libexec/debian-apt \
	-DDPKG_DATADIR=/usr/share/debian-apt/dpkg \
	-DREQUIRE_MERGED_USR=OFF \
	-DUSE_NLS=OFF -DWITH_DOC=OFF -DWITH_TESTS=OFF \
	-DCMAKE_BUILD_TYPE=Release

cmake --build "$work/build" \
	--target apt-cache apt-get file copy store gpgv http mirror rred -j"$JOBS"

copy "$work/build/cmdline/apt-cache" bin/apt-cache
copy "$work/build/cmdline/apt-get" bin/apt-get

for method in file copy store gpgv http mirror rred; do
	copy "$work/build/methods/$method" "usr/libexec/debian-apt/methods/$method"
done
copy "$work/build/methods/http" usr/libexec/debian-apt/methods/https

"$CC" -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -static \
	"-DPROVIDER_SHA256=\"$provider_sha\"" \
	"-DLOADER_SHA256=\"$loader_sha\"" \
	"-DLIBC_SHA256=\"$libc_sha\"" \
	-o "$work/debian-apt" "$port/debian-apt.c"
copy "$work/debian-apt" bin/debian-apt
copy "$work/debian-apt" usr/libexec/debian-apt/dpkg-blocked
copy "$work/synapse-glibc-runtime.deb" usr/share/debian-apt/synapse-glibc-runtime.deb

copy "$port/apt.conf" etc/debian-apt/apt.conf
copy "$port/sources.list" etc/debian-apt/sources.list
mkdir -p "$payload/etc/debian-apt"
: > "$payload/etc/debian-apt/status"
printf '%s\n' 'amd64' > "$payload/etc/debian-apt/arch"
mkdir -p "$payload/etc/debian-apt/log"
: > "$payload/etc/debian-apt/log/.keep"
mkdir -p "$payload/etc/debian-apt/apt.conf.d" "$payload/etc/debian-apt/preferences.d" \
	"$payload/etc/debian-apt/log"
: > "$payload/etc/debian-apt/apt.conf.d/.keep"
: > "$payload/etc/debian-apt/preferences.d/.keep"

copy "$source/COPYING.GPL" usr/share/licenses/apt/COPYING.GPL
