# recipe.sh: сборка Synapse Link SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl
# Подсистема: пользовательские приложения
# Первое изменение: 27.11.2026, последнее изменение: 27.11.2026
# Статус доступа: общедоступный

if [[ $TARGET_ARCH != x86_64 ]]; then
    echo "Synapse Link требует настольный USB-драйвер x86_64" >&2
    exit 1
fi

qt="$BUILD_ROOT/qt6-base/stage"
runtime="$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime"
runtime+=":$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm:$qt/usr/lib"
runtime+=":$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"
cxx=(bash "$ROOT/build/cxx-shared.sh")

mkdir -p "$payload/usr/libexec/qt6" "$payload/bin" "$payload/usr/share/applications"

"${cxx[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -D__synapse__ \
    -I"$port" \
    -I"$qt/usr/include/qt6" \
    -I"$qt/usr/include/qt6/QtWidgets" \
    -I"$qt/usr/include/qt6/QtGui" \
    -I"$qt/usr/include/qt6/QtCore" \
    "$port/main.cpp" -L"$qt/usr/lib" \
    -Wl,-rpath-link,"$runtime" -Wl,--no-allow-shlib-undefined \
    -lQt6Widgets -lQt6Gui -lQt6DBus -lQt6Core \
    -o "$payload/usr/libexec/qt6/synapse-link"

"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
    '-DAPPLICATION_PATH="/usr/libexec/qt6/synapse-link"' \
    "$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/synapse-link"

cp "$port/synapse-link.desktop" "$payload/usr/share/applications/"
