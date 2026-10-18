# recipe.sh: упаковать Qt Wayland с адаптированными библиотеками SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: графические пакеты
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный, MIT

source "$ROOT/pkgs/qt6-base/runtime.sh"
qt_import_assets

for file in "$work/stage/usr/lib/"libQt6*.so.6; do
	copy "$file" "usr/lib/qt6/runtime/$(basename "$file")"
done

copy "$work/stage/usr/lib/qt6/plugins/platforms/libqwayland.so" usr/lib/qt6/plugins/platforms/libqwayland.so
copy "$work/stage/usr/lib/qt6/plugins/wayland-shell-integration/libxdg-shell.so" \
	usr/lib/qt6/plugins/wayland-shell-integration/libxdg-shell.so
copy "$work/stage/usr/lib/qt6/plugins/wayland-decoration-client/libbradient.so" \
	usr/lib/qt6/plugins/wayland-decoration-client/libbradient.so
qt_runtime_dependencies usr/lib/qt6/runtime
qt_remove_provided "$payload/usr/lib/qt6/runtime" "$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime"
qt_remove_provided "$payload/usr/lib/qt6/runtime" "$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm"
qt_license_metadata
copy "$port/README.md" usr/share/doc/qt6-wayland/README.md
