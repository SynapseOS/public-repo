# recipe.sh: собрать D-Bus для рабочего стола SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: сеансовая шина без системных служб Linux
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare
export SYNAPSE_LINK_MODE=dynamic

expat="$BUILD_ROOT/expat/payload/usr"
[[ -f $expat/lib/libexpat.a ]]

sysroot=$(mktemp -d /tmp/synapse-dbus-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig"
cp "$expat/include/expat.h" "$expat/include/expat_external.h" "$sysroot/usr/include/"
cp "$expat/lib/libexpat.a" "$expat/lib/pkgconfig/expat.pc" "$sysroot/usr/lib/"
mv "$sysroot/usr/lib/expat.pc" "$sysroot/usr/lib/pkgconfig/expat.pc"

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
c_args = ['-O2', '-D__synapse__', '-DXML_GE=1', '-I$sysroot/usr/include']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

rm -rf -- "$work/build"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" \
	--prefix=/usr --bindir=/bin --libdir=lib --buildtype=release \
	-Ddefault_library=static -Dmessage_bus=true -Dmodular_tests=disabled \
	-Dasserts=true -Dchecks=true \
	-Dapparmor=disabled -Dselinux=disabled -Dlibaudit=disabled \
	-Dsystemd=disabled -Dlaunchd=disabled -Dkqueue=disabled \
	-Depoll=disabled -Dinotify=disabled -Dx11_autolaunch=disabled \
	-Dxml_docs=disabled -Ddoxygen_docs=disabled -Dducktype_docs=disabled \
	-Dqt_help=disabled -Dinstalled_tests=false -Dtools=true

ninja -C "$work/build" -j"$JOBS" bus/dbus-daemon tools/dbus-run-session tools/dbus-uuidgen
copy "$work/build/bus/dbus-daemon" bin/dbus-daemon
copy "$work/build/tools/dbus-run-session" bin/dbus-run-session
copy "$work/build/tools/dbus-uuidgen" bin/dbus-uuidgen
copy "$work/build/bus/session.conf" usr/share/dbus-1/session.conf
copy "$source/COPYING" usr/share/licenses/dbus/COPYING
