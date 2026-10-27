# recipe.sh: собрать Bubblewrap без набора привилегий хоста
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: подготовить bwrap для гостевого теста пространств имён
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 27.10.2026
# Статус доступа: общедоступный

prepare
export SYNAPSE_LINK_MODE=dynamic

# Отдельное пространство сети SynapseOS уже закрывает весь IPv4
# Настройка Linux loopback через NETLINK_ROUTE здесь неприменима
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/bubblewrap.c
+++ b/bubblewrap.c
@@ -3247,7 +3247,9 @@
   switch_to_user_with_privs ();
 
+#ifndef __synapse__
   if (opt_unshare_net)
     loopback_setup (); /* Will exit if unsuccessful */
+#endif
 
   ns_uid = opt_sandbox_uid;
   ns_gid = opt_sandbox_gid;
PATCH

fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"

sysroot=$(mktemp -d /tmp/synapse-bwrap-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include/sys" "$sysroot/usr/include/linux" \
	"$sysroot/usr/lib/pkgconfig"
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$sysroot"
ln -s "$sysroot/usr/include/x86_64-linux-gnu/asm" "$sysroot/usr/include/asm"
cp "$BUILD_ROOT/libcap/payload/usr/include/sys/capability.h" "$sysroot/usr/include/sys/"
cp "$BUILD_ROOT/libcap/payload/usr/include/linux/capability.h" "$sysroot/usr/include/linux/"
cp "$BUILD_ROOT/libcap/payload/usr/lib/libcap.a" "$sysroot/usr/lib/"
cp "$BUILD_ROOT/libcap/payload/usr/lib/pkgconfig/libcap.pc" "$sysroot/usr/lib/pkgconfig/"

cat >"$work/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-O2', '-D__synapse__', '-I$sysroot/usr/include']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

rm -rf -- "$work/build"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" \
	--prefix=/usr --bindir=/usr/bin --libdir=lib --buildtype=release \
	-Dtests=false -Dman=disabled -Dselinux=disabled \
	-Dbash_completion=disabled -Dzsh_completion=disabled

ninja -C "$work/build" -j"$JOBS"
copy "$work/build/bwrap" usr/bin/bwrap
copy "$source/COPYING.LIB" usr/share/licenses/bubblewrap/COPYING.LIB
