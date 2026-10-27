# recipe.sh: собрать libxmlb для AppStream
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: чтение двоичного индекса метаданных приложений
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 26.10.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d /tmp/synapse-libxmlb-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig"

for package in glib glib-gio libffi pcre2 zlib liblzma; do
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
c_args = ['-O2', '-fPIC', '-D__synapse__', '-isystem', '$(gcc -print-file-name=include)']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

meson setup "$work/build" "$source" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
	-Ddefault_library=static -Dtests=false -Dgtkdoc=false -Dintrospection=false \
	-Dcli=false -Dstemmer=false -Dlzma=enabled -Dzstd=disabled

ninja -C "$work/build" -j"$JOBS" src/libxmlb.a
copy "$work/build/src/libxmlb.a" usr/lib/libxmlb.a

for header in "$source/src/"*.h; do
	case $(basename "$header") in
		xmlb.h | xb-*.h)
			copy "$header" "usr/include/libxmlb/$(basename "$header")"
			;;
	esac
done

copy "$work/build/src/libxmlb/xb-version.h" usr/include/libxmlb/xb-version.h
copy "$source/LICENSE" usr/share/licenses/libxmlb/LICENSE

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/xmlb.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: xmlb
Description: Binary XML library
Version: 0.3.29
Requires: gio-2.0 liblzma
Libs: -L${libdir} -lxmlb
Cflags: -I${includedir}/libxmlb
PC
