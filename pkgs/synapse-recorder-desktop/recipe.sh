# SynapseOS, сборка настольной студии записи, 21.11.2026
qt="$BUILD_ROOT/qt6-base/stage"
lab="$BUILD_ROOT/labwc"
runtime="$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime:$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm"
runtime+=":$qt/usr/lib:$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"
mkdir -p "$payload/usr/libexec/qt6" "$payload/bin" "$payload/usr/share/applications"
SYNAPSE_LINK_MODE=dynamic bash "$ROOT/build/cxx-shared.sh" -std=c++20 -O2 -Wall -Wextra -Werror \
    -I"$qt/usr/include/qt6" -I"$qt/usr/include/qt6/QtWidgets" -I"$qt/usr/include/qt6/QtGui" \
    -I"$qt/usr/include/qt6/QtCore" "$port/main.cpp" -L"$qt/usr/lib" -Wl,-rpath-link,"$runtime" \
    -lQt6Widgets -lQt6Gui -lQt6Core -lQt6DBus -o "$payload/usr/libexec/qt6/recorder-desktop"
"$CC" -O2 -Wall -Wextra -Werror '-DAPPLICATION_PATH="/usr/libexec/qt6/recorder-desktop"' \
    "$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/recorder-desktop"
winxml="$ROOT/pkgs/labwc/synapse-window-capture.xml"
wayland-scanner client-header "$winxml" "$work/window-capture.h"
wayland-scanner private-code "$winxml" "$work/window-capture.c"
xml="$lab/wlroots-0.19.3/protocol/wlr-screencopy-unstable-v1.xml"
wayland-scanner client-header "$xml" "$work/screencopy.h"
wayland-scanner private-code "$xml" "$work/screencopy.c"
SYNAPSE_LINK_MODE=dynamic "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$work" -I"$lab/sdk/usr/include" \
    "$port/grab.c" "$work/screencopy.c" "$work/window-capture.c" -L"$lab/sdk/usr/lib" -Wl,-rpath-link,"$lab/sdk/usr/lib" \
    -Wl,-rpath,/usr/lib/labwc/runtime:/usr/lib/weston/runtime:/usr/lib -lwayland-client \
    -o "$payload/usr/libexec/recorder-grab"
cp "$port/recorder.desktop" "$payload/usr/share/applications/synapse-recorder.desktop"
SYNAPSE_LINK_MODE=dynamic bash "$ROOT/build/cxx-shared.sh" -std=c++20 -O2 -Wall -Wextra -Werror \
    -I"$qt/usr/include/qt6" -I"$qt/usr/include/qt6/QtWidgets" -I"$qt/usr/include/qt6/QtGui" \
    -I"$qt/usr/include/qt6/QtCore" "$port/media-app.cpp" -L"$qt/usr/lib" -Wl,-rpath-link,"$runtime" \
    -lQt6Widgets -lQt6Gui -lQt6Core -lQt6DBus -o "$payload/usr/libexec/qt6/synapse-camera"
ln "$payload/usr/libexec/qt6/synapse-camera" "$payload/usr/libexec/qt6/synapse-dictaphone"
mkdir -p "$payload/usr/share/icons/hicolor/128x128/apps"
for app in synapse-camera synapse-dictaphone; do
    "$CC" -O2 -Wall -Wextra -Werror "-DAPPLICATION_PATH=\"/usr/libexec/qt6/$app\"" \
        "$ROOT/pkgs/qt6-base/launch.c" -o "$payload/bin/$app"
    cp "$port/$app.desktop" "$payload/usr/share/applications/"
done
cp "$port/"*.png "$payload/usr/share/icons/hicolor/128x128/apps/"
