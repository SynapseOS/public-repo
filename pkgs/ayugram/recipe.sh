# recipe.sh: собрать AyuGram и подготовить его частные библиотеки musl
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: графические пакеты
# Первое изменение: 18.10.2026, последнее изменение: 19.10.2026
# Статус доступа: общедоступный, MIT

source "$ROOT/pkgs/qt6-base/runtime.sh"
bash "$port/source-build.sh"
qt_import_assets
copy "$BUILD_ROOT/ayugram-source/out/bin/AyuGram" usr/libexec/ayugram/AyuGram
chmod 755 "$payload/usr/libexec/ayugram/AyuGram"
copy "$BUILD_ROOT/ayugram-source/out/ayugram-sqlite-probe" bin/ayugram-sqlite-probe
chmod 755 "$payload/bin/ayugram-sqlite-probe"
copy "$BUILD_ROOT/ayugram-source/out/ayugram-async-probe" usr/libexec/ayugram/async-probe
chmod 755 "$payload/usr/libexec/ayugram/async-probe"
qt_runtime_dependencies usr/lib/ayugram/runtime
qt_remove_provided "$payload/usr/lib/ayugram/runtime" "$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime"
qt_remove_provided "$payload/usr/lib/ayugram/runtime" "$BUILD_ROOT/qt6-wayland/payload/usr/lib/qt6/runtime"
qt_remove_provided "$payload/usr/lib/ayugram/runtime" "$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm"
"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
	'-DAPPLICATION_PATH="/usr/libexec/ayugram/AyuGram"' \
	"$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/ayugram"
"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
	'-DAPPLICATION_PATH="/usr/libexec/ayugram/async-probe"' \
	"$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/ayugram-async-probe"
copy "$BUILD_ROOT/ayugram-source/ayu-src/lib/xdg/com.ayugram.desktop.desktop" \
	usr/share/applications/com.ayugram.desktop.desktop
sed -i -e 's|^TryExec=.*|TryExec=/bin/ayugram|' \
	-e 's|^Exec=.*-- %U$|Exec=/bin/ayugram -- %U|' \
	-e 's|^Exec=AyuGram -quit$|Exec=/bin/ayugram -quit|' \
	-e 's|^DBusActivatable=true$|DBusActivatable=false|' \
	"$payload/usr/share/applications/com.ayugram.desktop.desktop"
copy "$BUILD_ROOT/ayugram-source/ayu-src/Telegram/Resources/art/icon64.png" \
	usr/share/icons/hicolor/64x64/apps/com.ayugram.desktop.png
chmod 644 "$payload/usr/share/applications/com.ayugram.desktop.desktop" \
	"$payload/usr/share/icons/hicolor/64x64/apps/com.ayugram.desktop.png"

qt_license_metadata
copy "$BUILD_ROOT/ayugram-source/ayu-src/LICENSE" usr/share/licenses/ayugram/LICENSE
copy "$BUILD_ROOT/ayugram-source/ayu-src/LEGAL" usr/share/licenses/ayugram/LEGAL
copy "$BUILD_ROOT/ayugram-source/tde2e-src/LICENSE_1_0.txt" usr/share/licenses/ayugram/tde2e-LICENSE.txt
copy "$port/port.json" usr/share/licenses/ayugram/source.json
find "$payload/usr/share/licenses/ayugram" -type f -exec chmod 644 {} +
copy "$port/README.md" usr/share/doc/ayugram/README.md
