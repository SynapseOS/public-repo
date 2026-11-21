# recipe.sh: Собрать меню программ и графические настройки SynapseOS
# Автор: Елчинян Арен Артурович, 2026, архитектуры x86_64 и AArch64, ABI musl
# Подсистема: графические пакеты
# Первое изменение: 25.10.2026, последнее изменение: 21.11.2026
# Статус доступа: общедоступный

probe="$BUILD_ROOT/qt6-base/stage"
linkpaths="$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime"
linkpaths+=":$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm"
linkpaths+=":$probe/usr/lib:$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"

mkdir -p "$payload/usr/libexec/qt6"

cxx=(bash "$ROOT/build/cxx-shared.sh")
if [[ $TARGET_ARCH == aarch64 ]]; then
    probe=${ARM64_DESKTOP_SYSROOT:-$probe}
    gcc_object=$(find "$probe/usr/lib/gcc/aarch64-alpine-linux-musl" -name crtbeginS.o -print -quit)
    test -f "$gcc_object"
    cxx=("${ARM64_CXX:-clang++}" --target=aarch64-alpine-linux-musl "--sysroot=$probe"
        "--gcc-install-dir=$(dirname "$gcc_object")" -fuse-ld=lld -fPIC -pie)
    linkpaths="$probe/usr/lib:$probe/lib"
fi

"${cxx[@]}" \
	-std=c++20 -O2 -Wall -Wextra -Werror \
	-I"$SDK/include" \
	-I"$probe/usr/include/qt6" \
	-I"$probe/usr/include/qt6/QtWidgets" \
	-I"$probe/usr/include/qt6/QtGui" \
	-I"$probe/usr/include/qt6/QtCore" \
	"$port/main.cpp" -L"$probe/usr/lib" \
	-Wl,-rpath-link,"$linkpaths" -Wl,--no-allow-shlib-undefined \
	-lQt6Widgets -lQt6Gui -lQt6Core -lQt6DBus \
	-o "$payload/usr/libexec/qt6/synapse-desktop"

launcher_flags=()
if [[ $TARGET_ARCH == aarch64 ]]; then
    # Библиотеки отложенной загрузки Mesa поставляет зависимость synapse-files
    launcher_flags+=('-DQT_APP_LIBRARY_PATH="/usr/lib/synapse-files:"')
fi
"$CC" "${launcher_flags[@]}" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
	"-DAPPLICATION_PATH=\"/usr/libexec/qt6/synapse-desktop\"" \
	"$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/synapse-desktop"

"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
	"$port/menu-toggle.c" -o "$payload/bin/synapse-menuctl"

pack
