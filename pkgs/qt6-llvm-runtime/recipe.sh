# recipe.sh: упаковать отдельный LLVM runtime для библиотек Mesa в Qt
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: графические пакеты
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный, MIT

source "$ROOT/pkgs/qt6-base/runtime.sh"
qt_import_assets
copy "$work/stage/usr/lib/libLLVM.so.20.1" usr/lib/qt6/llvm/libLLVM.so.20.1
qt_runtime_dependencies usr/lib/qt6/llvm
qt_license_metadata
copy "$port/README.md" usr/share/doc/qt6-llvm-runtime/README.md
