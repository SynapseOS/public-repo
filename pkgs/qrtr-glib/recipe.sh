# recipe.sh: настоящая статическая libqrtr-glib для QRTR транспорта
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64 / ARM64, ABI: synapse-x86_64-v1 / synapse-aarch64-v1
# Подсистема: сборка пакетов
# Первое изменение: 25.11.2026, последнее изменение: 25.11.2026
# Статус доступа: общедоступный
prepare
source "$ROOT/build/qmi-meson.sh"
qmi_cross_setup
rm -rf -- "$work/build" "$work/install"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" --prefix=/usr --libdir=lib \
	--wrap-mode=nofallback -Dbuildtype=release -Ddefault_library=static -Dintrospection=false -Dgtk_doc=false
ninja -C "$work/build" -j"$JOBS"
DESTDIR="$work/install" meson install -C "$work/build" --no-rebuild
qmi_copy_install
for license in "$source/LICENSES/"*; do
	copy "$license" "usr/share/licenses/qrtr-glib/$(basename "$license")"
done
