# recipe.sh: сборка агентской среды SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl
# Подсистема: пользовательские приложения
# Первое изменение: 24.11.2026, последнее изменение: 24.11.2026
# Статус доступа: общедоступный

qt="$BUILD_ROOT/qt6-base/stage"
runtime="$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime"
runtime+=":$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm:$qt/usr/lib"
runtime+=":$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"
cxx=(bash "$ROOT/build/cxx-shared.sh")

mkdir -p "$payload/usr/libexec/qt6" "$payload/bin" "$payload/usr/share/applications"

# Список исходных файлов для компиляции
sources=(
    "$port/main.cpp"
    "$port/ai_provider.cpp"
    "$port/agent_config.cpp"
    "$port/chat_agent.cpp"
    "$port/os_tools.cpp"
    "$port/main_window.cpp"
)

"${cxx[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -D__synapse__ \
    -I"$port" \
    -I"$qt/usr/include/qt6" \
    -I"$qt/usr/include/qt6/QtWidgets" \
    -I"$qt/usr/include/qt6/QtGui" \
    -I"$qt/usr/include/qt6/QtCore" \
    -I"$qt/usr/include/qt6/QtNetwork" \
    "${sources[@]}" -L"$qt/usr/lib" \
    -Wl,-rpath-link,"$runtime" -Wl,--no-allow-shlib-undefined \
    -lQt6Widgets -lQt6Gui -lQt6Network -lQt6DBus -lQt6Core \
    -o "$payload/usr/libexec/qt6/synapse-agent"

"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
    '-DAPPLICATION_PATH="/usr/libexec/qt6/synapse-agent"' \
    "$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/synapse-agent"

cp "$port/synapse-agent.desktop" "$payload/usr/share/applications/"
pack
