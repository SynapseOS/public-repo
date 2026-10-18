# recipe.sh: подготовить musl Qt и собрать проверку Qt Widgets
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: графические пакеты
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный, MIT

source "$port/runtime.sh"
qt_import_assets

for file in "$work/stage/usr/lib/"libQt6*.so.6; do
	case "$(basename "$file")" in
		*Wayland* | *WlShell*)
			continue
			;;
	esac

	copy "$file" "usr/lib/qt6/runtime/$(basename "$file")"
done

for directory in imageformats iconengines tls; do
	for file in "$work/stage/usr/lib/qt6/plugins/$directory/"*.so; do
		copy "$file" "usr/lib/qt6/plugins/$directory/$(basename "$file")"
	done
done

for plugin in libqoffscreen.so libqminimal.so; do
	copy "$work/stage/usr/lib/qt6/plugins/platforms/$plugin" "usr/lib/qt6/plugins/platforms/$plugin"
done

mkdir -p "$payload/usr/share/fonts"
cp -rL "$work/stage/usr/share/fonts/opensans" "$payload/usr/share/fonts/"
qt_runtime_dependencies usr/lib/qt6/runtime
qt_remove_provided "$payload/usr/lib/qt6/runtime" "$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm"
ln -sfn "$SDK/apps/musl/lib/libc.so" "$work/stage/usr/lib/libc.musl-x86_64.so.1"
probe="$work/stage"
cppflags=(-std=c++20 -O2 -Wall -Wextra -Werror
	-I"$probe/usr/include/qt6" -I"$probe/usr/include/qt6/QtWidgets"
	-I"$probe/usr/include/qt6/QtGui" -I"$probe/usr/include/qt6/QtCore")
linkpaths="$payload/usr/lib/qt6/runtime:$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm"
linkpaths+=":$probe/usr/lib:$probe/usr/lib/libproxy:$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"
mkdir -p "$payload/usr/libexec/qt6"
bash "$ROOT/build/cxx-shared.sh" "${cppflags[@]}" "$port/smoke.cpp" \
	-L"$probe/usr/lib" -Wl,-rpath-link,"$linkpaths" -Wl,--no-allow-shlib-undefined \
	-lQt6Widgets -lQt6Gui -lQt6Core -lQt6DBus -o "$payload/usr/libexec/qt6/qt6-smoke"
"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none "$port/launch.c" -o "$payload/bin/qt6-smoke"

for test in shm-probe local-socket-probe wait-probe; do
	"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none "$port/$test.c" -o "$payload/bin/qt6-$test"
done

for test in process-probe network-probe; do
	bash "$ROOT/build/cxx-shared.sh" "${cppflags[@]}" -I"$probe/usr/include/qt6/QtNetwork" \
		"$port/$test.cpp" -L"$probe/usr/lib" -Wl,-rpath-link,"$linkpaths" \
		-Wl,--no-allow-shlib-undefined -lQt6Network -lQt6Core -o "$payload/usr/libexec/qt6/$test"
	"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none \
		"-DAPPLICATION_PATH=\"/usr/libexec/qt6/$test\"" "$port/launch.c" -o "$payload/bin/qt6-$test"
done

qt_license_metadata
copy "$port/README.md" usr/share/doc/qt6-base/README.md
