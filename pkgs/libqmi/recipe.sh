# recipe.sh: полный libqmi и qmicli с собственным QRTR транспортом
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64 / ARM64, ABI: synapse-x86_64-v1 / synapse-aarch64-v1
# Подсистема: сборка пакетов
# Первое изменение: 25.11.2026, последнее изменение: 25.11.2026
# Статус доступа: общедоступный
prepare
source "$ROOT/build/qmi-meson.sh"
qmi_cross_setup qrtr-glib
rm -rf -- "$work/build" "$work/install"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" --prefix=/usr --libdir=lib \
	--wrap-mode=nofallback -Dbuildtype=release -Ddefault_library=static -Dcollection=full \
	-Dintrospection=false -Dgtk_doc=false -Dman=false -Dbash_completion=false \
	-Dfirmware_update=false -Dmbim_qmux=false -Dudev=false -Dqrtr=true -Drmnet=false -Dmm_runtime_check=false
ninja -C "$work/build" -j"$JOBS" src/qmicli/qmicli src/qmi-proxy/qmi-proxy
# Установка только production-целей: тесты не запускаются на машине другой архитектуры.
DESTDIR="$work/install" meson install -C "$work/build" --no-rebuild --tags devel,runtime
qmi_copy_install
copy "$source/COPYING" usr/share/licenses/libqmi/COPYING
copy "$source/COPYING.LIB" usr/share/licenses/libqmi/COPYING.LIB
