# recipe.sh: собрать библиотеку метаданных AppStream
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: чтение описаний приложений для Flatpak
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

# Meson 1.7 возвращает void из i18n.gettext
sed -i 's/^i18n_result = i18n.gettext(/i18n.gettext(/' "$source/po/meson.build"

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-appstream-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig"
mkdir -p "$sysroot/usr/bin"

for package in glib glib-gio libffi pcre2 zlib libcurl openssl-pic libxml2 libfyaml libxmlb liblzma; do
	stage="$BUILD_ROOT/$package/payload/usr"
	[[ -d $stage ]]
	cp -a "$stage/." "$sysroot/usr/"
done

for tool in glib-mkenums glib-genmarshal glib-compile-resources; do
	ln -s "$(command -v "$tool")" "$sysroot/usr/bin/$tool"
done

printf '\nglib_mkenums=/usr/bin/glib-mkenums\nglib_genmarshal=/usr/bin/glib-genmarshal\n' \
	>>"$sysroot/usr/lib/pkgconfig/glib-2.0.pc"
printf '\nglib_compile_resources=/usr/bin/glib-compile-resources\n' \
	>>"$sysroot/usr/lib/pkgconfig/gio-2.0.pc"

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
	-Ddefault_library=static -Dstemming=false -Dsystemd=false -Dgir=false \
	-Dzstd-support=false -Dqt=false -Dcompose=false -Ddocs=false -Dman=false \
	-Dbash-completion=false -Dsvg-support=false

ninja -C "$work/build" -j"$JOBS" src/libappstream.a
copy "$work/build/src/libappstream.a" usr/lib/libappstream.a

for header in "$source/src/"*.h; do
	case $(basename "$header") in
		appstream.h | as-*.h)
			copy "$header" "usr/include/appstream/$(basename "$header")"
			;;
	 esac
done

copy "$work/build/src/as-version.h" usr/include/appstream/as-version.h
copy "$work/build/src/as-enum-types.h" usr/include/appstream/as-enum-types.h
copy "$source/COPYING" usr/share/licenses/appstream/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/appstream.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: appstream
Description: AppStream metadata library
Version: 1.1.2
Requires: glib-2.0 gio-2.0 gobject-2.0
Requires.private: libcurl libxml-2.0 libfyaml xmlb
Libs: -L${libdir} -lappstream
Cflags: -I${includedir}/appstream
PC
