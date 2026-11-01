# recipe.sh: собрать отдельный графический магазин программ
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI synapse-x86_64-v1
# Подсистема: графические пакеты
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

probe="$BUILD_ROOT/qt6-base/stage"
linkpaths="$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime"
linkpaths+=":$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm"
linkpaths+=":$probe/usr/lib:$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"

mkdir -p "$payload/usr/libexec/qt6" "$payload/usr/share/applications"
cp "$port/synapse-store.desktop" "$payload/usr/share/applications/synapse-store.desktop"

bash "$ROOT/build/cxx-shared.sh" \
	-std=c++20 -O2 -Wall -Wextra -Werror \
	-I"$probe/usr/include/qt6" \
	-I"$probe/usr/include/qt6/QtWidgets" \
	-I"$probe/usr/include/qt6/QtGui" \
	-I"$probe/usr/include/qt6/QtCore" \
	"$port/main.cpp" -L"$probe/usr/lib" \
	-Wl,-rpath-link,"$linkpaths" -Wl,--no-allow-shlib-undefined \
	-lQt6Widgets -lQt6Gui -lQt6Core -lQt6DBus \
	-o "$payload/usr/libexec/qt6/synapse-store"

"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
	"-DAPPLICATION_PATH=\"/usr/libexec/qt6/synapse-store\"" \
	"$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/synapse-store"

"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
	"$port/store_control.c" -o "$payload/bin/synapse-store-control"

"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
	"$port/store_service.c" -o "$payload/bin/synapse-store-helper"

copy "$port/service" etc/services/synapse-store-helper

pack
