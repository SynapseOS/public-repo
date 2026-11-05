# recipe.sh: собрать портал открытия ссылок Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: собрать пользовательский сервис с библиотекой GIO
# Подсистема: сборка пакетов
# Первое изменение: 29.10.2026
# Последнее изменение: 29.10.2026
# Доступ: общедоступный

export SYNAPSE_LINK_MODE=dynamic

sysroot="$work/sysroot"
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig"
for package in glib glib-gio libffi pcre2 zlib; do
	stage="$BUILD_ROOT/$package/payload/usr"
	[[ -d $stage ]]
	cp -a "$stage/." "$sysroot/usr/"
done

alias_dir=$(mktemp -d "${TMPDIR:-/tmp}/synapse-openuri.XXXXXX")
ln -s "$sysroot" "$alias_dir/sdk"
trap 'rm -f -- "$alias_dir/sdk"; rmdir -- "$alias_dir"' EXIT

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$alias_dir/sdk"

read -r -a cflags <<<"$(pkg-config --cflags gio-2.0)"
read -r -a libraries <<<"$(pkg-config --static --libs gio-2.0)"

"$CC" -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror \
	"${cflags[@]}" "$port/portal.c" "${libraries[@]}" \
	-o "$work/synapse-openuri-portal"

copy "$work/synapse-openuri-portal" usr/libexec/synapse-openuri-portal
copy "$port/org.freedesktop.portal.Desktop.service" \
	usr/share/dbus-1/services/org.freedesktop.portal.Desktop.service
