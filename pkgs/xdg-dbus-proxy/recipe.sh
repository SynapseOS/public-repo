# recipe.sh: собрать фильтр сообщений D-Bus для Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: прокси отделяет шину приложений от системной шины
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 26.10.2026
# Статус доступа: общедоступный

prepare
export SYNAPSE_LINK_MODE=dynamic

sysroot=$(mktemp -d /tmp/synapse-dbus-proxy-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig"

for package in glib glib-gio libffi pcre2 zlib; do
	stage="$BUILD_ROOT/$package/payload/usr"
	[[ -d $stage ]]
	cp -a "$stage/." "$sysroot/usr/"
done

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
c_args = ['-O2', '-D__synapse__']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

meson setup "$work/build" "$source" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --bindir=/bin --buildtype=release --wrap-mode=nofallback \
	-Dtests=false -Dman=disabled

ninja -C "$work/build" -j"$JOBS"
copy "$work/build/xdg-dbus-proxy" bin/xdg-dbus-proxy
copy "$source/COPYING" usr/share/licenses/xdg-dbus-proxy/COPYING
