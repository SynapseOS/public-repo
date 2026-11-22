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
mkdir -p "$provider/DEBIAN" "$provider/usr/lib" "$provider/usr/lib/x86_64-linux-gnu" \
	"$provider/usr/lib64" "$provider/usr/bin" "$provider/usr/sbin" \
	"$provider/usr/lib/locale" "$provider/usr/lib/gconv" \
	"$provider/usr/lib/synapse-runtime/bin" \
	"$provider/usr/share/debian-apt/dpkg" \
	"$provider/usr/share/doc/synapse-glibc-runtime"

# usrmerge layout Debian 13
ln -s usr/bin "$provider/bin"
ln -s usr/sbin "$provider/sbin"
ln -s usr/lib "$provider/lib"
ln -s usr/lib64 "$provider/lib64"

for library in "$glibc_stage"/lib/lib*.so* "$glibc_stage"/lib/ld-linux-x86-64.so.2; do
	if [[ -f $library ]]; then
		cp -a "$library" "$provider/usr/lib/"
		ln -sf "/lib/${library##*/}" "$provider/usr/lib/x86_64-linux-gnu/${library##*/}"
	fi
done

if [[ -d $glibc_stage/usr/lib/gconv ]]; then
	cp -a "$glibc_stage/usr/lib/gconv/." "$provider/usr/lib/gconv/"
fi
if [[ -d $glibc_stage/usr/lib/locale ]]; then
	cp -a "$glibc_stage/usr/lib/locale/." "$provider/usr/lib/locale/"
fi

cp "$glibc_stage/usr/share/licenses/glibc/COPYING.LIB" \
	"$provider/usr/share/doc/synapse-glibc-runtime/"
cp "$glibc_stage/usr/share/licenses/glibc/LICENSES" \
	"$provider/usr/share/doc/synapse-glibc-runtime/"
ln -s ../lib/ld-linux-x86-64.so.2 "$provider/usr/lib64/ld-linux-x86-64.so.2"

# bootstrap-инструменты для maintainer scripts внутри chroot
runtime_bin="$provider/usr/lib/synapse-runtime/bin"

fetch_deb() {
	local name="$1" url="$2" sha="$3"
	local archive="$work/$name"

	curl --fail --location --retry 3 --output "$archive" "$url"
	printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
	mkdir -p "$work/extract-$name"
	bsdtar -xf "$archive" -C "$work/extract-$name"
	bsdtar -xf "$work/extract-$name/data.tar.xz" -C "$work/extract-$name"
}

fetch_deb dash.deb \
	https://deb.debian.org/debian/pool/main/d/dash/dash_0.5.12-12_amd64.deb \
	a8902cb6d8650134764a25fb80aec8589d858ef71ece1680a62e84816c37bb04
fetch_deb busybox-static.deb \
	https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.37.0-6+b9_amd64.deb \
	598a3fd92bdafc34cd81196b2952ad36e910e47a4ecf162f8ce5e8e262598e53
fetch_deb libc-bin.deb \
	https://deb.debian.org/debian/pool/main/g/glibc/libc-bin_2.41-12+deb13u5_amd64.deb \
	f30ee7381a9c6a3936165259bcd58e65f4b288e351606d200c8435724c5876dd
fetch_deb dpkg.deb \
	https://deb.debian.org/debian/pool/main/d/dpkg/dpkg_1.22.22_amd64.deb \
	3e800c6d75e8e709007ed1c356e5fa8509a75c1694fb0a1183709abc9cfdc1f3
fetch_deb coreutils.deb \
	https://deb.debian.org/debian/pool/main/c/coreutils/coreutils_9.7-3_amd64.deb \
	1299ab6f9389a288eb2f5f3dd222c26cc777b9a2d5ecb6ee4cbd340cebcdada2
fetch_deb libmd0.deb \
	https://deb.debian.org/debian/pool/main/libm/libmd/libmd0_1.1.0-2+b1_amd64.deb \
	7244ec3839b61fac0c1884fe08aaa040f26e8f1f35f1f5d3482eacefd30d1b44
fetch_deb libselinux1.deb \
	https://deb.debian.org/debian/pool/main/libs/libselinux/libselinux1_3.8.1-1_amd64.deb \
	68bb8d32bd8d6d7d2f5952a169db03d1484b46ae1e52abccdec42a19dccea5d5
fetch_deb libacl1.deb \
	https://deb.debian.org/debian/pool/main/a/acl/libacl1_2.3.2-2+b1_amd64.deb \
	08074f01e384bc07c0c2d79a58cf4a6523f71cf75d1808101c79617656c9a39d
fetch_deb libattr1.deb \
	https://deb.debian.org/debian/pool/main/a/attr/libattr1_2.5.2-3_amd64.deb \
	606b5ee12ea2786be607a17f40c1fb5e65c76ceaff66665bdf8f8c6c1b71d1fb
fetch_deb libgmp10.deb \
	https://deb.debian.org/debian/pool/main/g/gmp/libgmp10_6.3.0+dfsg-3_amd64.deb \
	d0d0265eb01770f17afd0f7c8c0622f80479dcfbbe13653a0debeec61464e622
fetch_deb libpcre2-8-0.deb \
	https://deb.debian.org/debian/pool/main/p/pcre2/libpcre2-8-0_10.46-1~deb13u3_amd64.deb \
	e226f661d918f04daf38cdbc4806b7ed7d6ef95c7eb0ade692fc350e31970040

cp "$work/extract-busybox-static.deb/usr/bin/busybox" "$runtime_bin/busybox"
# busybox sh предпочитает встроенные апплеты и обходит PATH
# для maintainer scripts нужен настоящий dash, иначе вместо GNU coreutils работают апплеты
cp "$work/extract-dash.deb/usr/bin/dash" "$runtime_bin/dash"

for tool in ldconfig; do
	cp "$work/extract-libc-bin.deb/usr/sbin/$tool" "$runtime_bin/$tool"
done
for tool in getent; do
	cp "$work/extract-libc-bin.deb/usr/bin/$tool" "$runtime_bin/$tool"
done
for tool in update-alternatives start-stop-daemon; do
	cp "$work/extract-dpkg.deb/usr/sbin/$tool" "$runtime_bin/$tool" 2>/dev/null || \
		cp "$work/extract-dpkg.deb/usr/bin/$tool" "$runtime_bin/$tool"
done

for tool in dpkg dpkg-query; do
	source_tool="$BUILD_ROOT/debian-dpkg/payload"
	if [[ -f $source_tool/usr/libexec/debian-apt/dpkg-native && $tool == dpkg ]]; then
		cp "$source_tool/usr/libexec/debian-apt/dpkg-native" "$runtime_bin/dpkg"
	elif [[ -f $source_tool/bin/$tool ]]; then
		cp "$source_tool/bin/$tool" "$runtime_bin/$tool"
	else
		echo "Missing musl $tool for provider bootstrap" >&2
		exit 1
	fi
done

for tool in dpkg-maintscript-helper dpkg-realpath; do
	if [[ -f $work/extract-dpkg.deb/usr/bin/$tool ]]; then
		cp "$work/extract-dpkg.deb/usr/bin/$tool" "$runtime_bin/$tool"
	else
		echo "Missing dpkg helper $tool" >&2
		exit 1
	fi
done

for tool in dpkg-divert dpkg-statoverride dpkg-trigger dpkg-deb gtar; do
	source_tool="$BUILD_ROOT/debian-dpkg/payload/usr/libexec/debian-apt/$tool"
	if [[ ! -f $source_tool ]]; then
		source_tool="$BUILD_ROOT/dpkg-deb/payload/bin/$tool"
	fi
	if [[ ! -f $source_tool ]]; then
		source_tool="$BUILD_ROOT/gnu-tar/payload/bin/$tool"
	fi
	if [[ ! -f $source_tool ]]; then
		echo "Missing bootstrap tool $tool" >&2
		exit 1
	fi
	cp "$source_tool" "$runtime_bin/$tool"
done

for table in cputable ostable tupletable; do
	cp "$BUILD_ROOT/debian-dpkg/payload/usr/share/debian-apt/dpkg/$table" \
		"$provider/usr/share/debian-apt/dpkg/"
done

for applet in sh rm cp mv mkdir rmdir chmod chown chgrp ln cat echo printf test '[' \
	sed grep egrep fgrep awk find sort uniq cut tr head tail wc uname basename \
	dirname readlink realpath touch true false sleep expr mktemp xargs id env \
	install stat date dd df du ls md5sum sha256sum sync tar gzip gunzip zcat \
	xz unxz bzip2 bunzip2; do
	ln -s busybox "$runtime_bin/$applet"
done

# Maintainer scripts Debian ждут GNU coreutils, busybox-апплеты их не покрывают
for gnu_tool in "$work/extract-coreutils.deb/usr/bin/"*; do
	tool_name="${gnu_tool##*/}"
	rm -f "$runtime_bin/$tool_name"
	cp -a "$gnu_tool" "$runtime_bin/$tool_name"
done

for lib_package in libmd0 libselinux1 libacl1 libattr1 libgmp10 libpcre2-8-0; do
	cp -a "$work/extract-$lib_package.deb/usr/lib/x86_64-linux-gnu/." \
		"$provider/usr/lib/x86_64-linux-gnu/"
done

ln -sf dash "$runtime_bin/sh"
ln -s ../lib/synapse-runtime/bin/gtar "$provider/usr/bin/gtar"

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
