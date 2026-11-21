# recipe.sh: Собрать Labwc и wlroots для вложенного Wayland-сеанса
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: Использовать системный Wayland SDK и закрытые библиотеки пакета
# Подсистема: сборка пакетов
# Первое изменение: 28.10.2026
# Последнее изменение: 21.11.2026
# Доступ: общедоступный

prepare

fetch "$(jq -er .wlroots_source "$spec")" "$(jq -er .wlroots_sha256 "$spec")" \
	"$work/wlroots.tar.gz"
fetch "$(jq -er .swaybg_source "$spec")" "$(jq -er .swaybg_sha256 "$spec")" \
	"$work/swaybg.tar.gz"
fetch "$(jq -er .themes_source "$spec")" "$(jq -er .themes_sha256 "$spec")" \
	"$work/openbox-themes.tar.gz"
fetch "$(jq -er .xwayland_source "$spec")" "$(jq -er .xwayland_sha256 "$spec")" \
	"$work/xwayland.tar.xz"

wlroots="$work/wlroots-0.19.3"
if [[ ! -d $wlroots ]]; then
	bsdtar -xf "$work/wlroots.tar.gz" -C "$work"
fi

cp "$port/synapse-headless.inc" "$wlroots/backend/headless/"
if ! grep -q synapse-headless.inc "$wlroots/backend/headless/output.c"; then
	patch --batch --fuzz=0 -d "$wlroots" -p1 -i "$port/patches/synapse-framebuffer.patch"
fi

if grep -q 'synapse_fb_fd >= 0 || !synapse_fb_open' "$wlroots/backend/headless/output.c"; then
	patch --batch --fuzz=0 -d "$wlroots" -p1 <<'PATCH'
--- a/backend/headless/output.c
+++ b/backend/headless/output.c
@@ -140,3 +140,7 @@
 	if (getenv("SYNAPSE_FB") != NULL) {
-		if (synapse_fb_fd >= 0 || !synapse_fb_open(&width, &height)) {
+		if (synapse_fb_fd >= 0) {
+			wlr_log(WLR_DEBUG, "SynapseOS framebuffer is already in use by this compositor");
+			return NULL;
+		}
+		if (!synapse_fb_open(&width, &height)) {
 			wlr_log(WLR_ERROR, "Cannot open SynapseOS framebuffer");
PATCH
fi

if grep -q 'synapse_fb_present(state)' "$wlroots/backend/headless/output.c"; then
	patch --batch --fuzz=0 -d "$wlroots" -p1 <<'PATCH'
--- a/backend/headless/output.c
+++ b/backend/headless/output.c
@@ -65,1 +65,1 @@
-			!synapse_fb_present(state)) {
+			!synapse_fb_present(wlr_output, state)) {
PATCH
fi

if ! grep -A5 'static bool output_set_cursor' "$wlroots/backend/headless/output.c" |
	grep -q 'return false'; then
	sed -i '/static bool output_set_cursor(/,/^}/ s/return true;/if (synapse_fb_fd >= 0) { return false; }\n\treturn true;/' \
		"$wlroots/backend/headless/output.c"
fi
if ! grep -A5 'static bool output_move_cursor' "$wlroots/backend/headless/output.c" |
	grep -q 'return false'; then
	sed -i '/static bool output_move_cursor(/,/^}/ s/return true;/if (synapse_fb_fd >= 0) { return false; }\n\treturn true;/' \
		"$wlroots/backend/headless/output.c"
fi

if ! grep -q 'synapse_fb_poll_mode, wlr_output' "$wlroots/backend/headless/output.c"; then
	patch --batch --fuzz=0 -d "$wlroots" -p1 -i "$port/patches/synapse-live-mode.patch"
fi

# Резервировать DRM lease только для основного узла, render node работает через GBM
if ! grep -q 'drmGetNodeTypeFromFd(drm_fd) == DRM_NODE_PRIMARY' \
	"$wlroots/render/allocator/allocator.c"; then
	patch --batch --fuzz=0 -d "$wlroots" -p1 <<'PATCH'
--- a/render/allocator/allocator.c
+++ b/render/allocator/allocator.c
@@ -37,1 +37,1 @@
-	if (drmIsMaster(drm_fd)) {
+	if (drmGetNodeTypeFromFd(drm_fd) == DRM_NODE_PRIMARY && drmIsMaster(drm_fd)) {
PATCH
fi

cp "$port/synapse-input.inc" "$wlroots/backend/headless/"
if ! grep -q synapse-input.inc "$wlroots/backend/headless/backend.c"; then
	patch --batch --fuzz=0 -d "$wlroots" -p1 -i "$port/patches/synapse-input.patch"
fi

[[ -d $BUILD_ROOT/weston/sdk/usr/lib/pkgconfig ]]
rm -rf "$work/sdk"
mkdir -p "$work/sdk"
cp -a "$BUILD_ROOT/weston/sdk/." "$work/sdk/"
cp -a "$BUILD_ROOT/mesa/wayland-sdk/usr/include/." "$work/sdk/usr/include/"
cp -a "$BUILD_ROOT/mesa/wayland-sdk/usr/lib/." "$work/sdk/usr/lib/"
cp -a "$BUILD_ROOT/mesa/stage/usr/include/GL/." "$work/sdk/usr/include/GL/"
cp "$BUILD_ROOT/mesa/stage/usr/include/gbm.h" "$work/sdk/usr/include/gbm.h"
cp -a "$BUILD_ROOT/mesa/stage/usr/lib/." "$work/sdk/usr/lib/"
cp -a "$BUILD_ROOT/mesa/llvm/root/usr/lib/libLLVM.so.19.1" "$work/sdk/usr/lib/"

while IFS=$'\t' read -r repository filename checksum; do
	archive="$work/xwayland-apks/$filename"
	fetch "https://dl-cdn.alpinelinux.org/alpine/v3.22/$repository/x86_64/$filename" \
		"$checksum" "$archive"
	mkdir -p "$work/xwayland-apk-stage"
	bsdtar -xf "$archive" -C "$work/xwayland-apk-stage"
	cp -a "$work/xwayland-apk-stage/usr/." "$work/sdk/usr/"
	rm -rf "$work/xwayland-apk-stage"
done < <(jq -r '.[] | [.repo, .filename, .sha256] | @tsv' \
	"$port/xwayland-dependencies.json")

if grep -q 'snprintf(path, sizeof(s.sun_path)' "$work/sdk/usr/include/X11/Xtrans/Xtranssock.c"; then
	patch --batch --fuzz=0 -d "$work/sdk" -p1 -i "$port/patches/xtrans-safe-socket-path.patch"
fi

alias_path="/tmp/synapse-public-labwc-$(printf '%s' "$ROOT" | sha256sum | cut -c1-16)"
if [[ ! -e $alias_path ]]; then
	ln -s "$work/sdk" "$alias_path"
fi

[[ $(realpath "$alias_path") == $(realpath "$work/sdk") ]]
export PKG_CONFIG_SYSROOT_DIR="$alias_path"
export PKG_CONFIG_LIBDIR="$alias_path/usr/lib/pkgconfig:$alias_path/usr/share/pkgconfig"

cat >"$work/cross.ini" <<CROSS
[binaries]
c = ['env', 'SYNAPSE_LINK_MODE=dynamic', '$CC']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-D_GNU_SOURCE', '-isystem', '$(gcc -print-file-name=include)',
    '-I$SDK/include', '-I$SDK/apps/apps/nano-x/upstream/src/drivers',
    '-idirafter', '$BUILD_ROOT/mesa/linux-headers/usr/include']
c_link_args = ['-L$alias_path/usr/lib', '-Wl,-rpath-link,$alias_path/usr/lib']
CROSS

meson setup "$work/wlroots-build" "$wlroots" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release '-Dbackends=[]' \
	'-Drenderers=gles2' '-Dallocators=gbm' -Dsession=disabled -Dxwayland=enabled \
	-Dexamples=false -Dauto_features=disabled -Dwerror=true

ninja -C "$work/wlroots-build" -j"$JOBS"
meson install -C "$work/wlroots-build" --destdir "$work/wlroots-stage"
cp -a "$work/wlroots-stage/usr/include/." "$work/sdk/usr/include/"
cp -a "$work/wlroots-stage/usr/lib/." "$work/sdk/usr/lib/"

labwc_link_args="-L$alias_path/usr/lib -Wl,-rpath-link,$alias_path/usr/lib"
labwc_link_args+=" -Wl,-l:libpangoft2-1.0.so.0.5600.3"
labwc_link_args+=" -Wl,--no-as-needed $alias_path/usr/lib/libLLVM.so.19.1"

meson setup "$work/labwc-build" "$source" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release -Dxwayland=enabled \
	-Dicon=disabled -Dsvg=disabled -Dauto_features=disabled -Dwerror=true \
	"-Dc_link_args=$labwc_link_args"

ninja -C "$work/labwc-build" -j"$JOBS"
copy "$work/labwc-build/labwc" usr/libexec/labwc/labwc
copy "$work/labwc-build/clients/labnag" usr/libexec/labwc/labnag
copy "$work/sdk/usr/bin/xkbcomp" bin/xkbcomp
copy "$work/wlroots-stage/usr/lib/libwlroots-0.19.so" \
	usr/lib/labwc/runtime/libwlroots-0.19.so
"$CC" -O2 -Wall -Wextra -Werror "$port/launch.c" -o "$work/labwc-launch"
copy "$work/labwc-launch" bin/labwc
copy "$source/LICENSE" usr/share/licenses/labwc/LICENSE
copy "$wlroots/LICENSE" usr/share/licenses/labwc/wlroots-LICENSE

xwayland="$work/xwayland-24.1.9"
if [[ ! -d $xwayland ]]; then
	bsdtar -xf "$work/xwayland.tar.xz" -C "$work"
fi

# Xwayland ищет программный DRI-драйвер по пути внутри гостевой системы
if grep -q "dri_dep.get_variable(pkgconfig : 'dridriverdir')" "$xwayland/include/meson.build"; then
	patch --batch --fuzz=0 -d "$xwayland" -p1 <<'PATCH'
--- a/include/meson.build
+++ b/include/meson.build
@@ -207,1 +207,1 @@
-    conf_data.set_quoted('DRI_DRIVER_PATH', dri_dep.get_variable(pkgconfig : 'dridriverdir'))
+    conf_data.set_quoted('DRI_DRIVER_PATH', '/usr/lib/dri')
PATCH
fi

meson setup "$work/xwayland-build" "$xwayland" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --bindir=/bin --buildtype=release -Dglamor=true -Dglx=true \
	-Ddri3=true -Ddrm=false -Dxvfb=false -Dxdmcp=false -Dsecure-rpc=false \
	-Dxwayland_ei=false -Dsha1=libmd -Doptimization=2 -Ddocs=false \
	-Ddevel-docs=false -Ddocs-pdf=false -Dauto_features=disabled -Dwerror=true \
	"-Dc_link_args=-L$alias_path/usr/lib -Wl,-rpath-link,$alias_path/usr/lib -Wl,--no-as-needed $alias_path/usr/lib/libLLVM.so.19.1"
ninja -C "$work/xwayland-build" -j"$JOBS"
copy "$work/xwayland-build/hw/xwayland/Xwayland" bin/Xwayland
copy "$xwayland/COPYING" usr/share/licenses/labwc/xwayland-COPYING

if [[ ! -d $work/swaybg-1.2.2 ]]; then
	bsdtar -xf "$work/swaybg.tar.gz" -C "$work"
fi

meson setup "$work/swaybg-build" "$work/swaybg-1.2.2" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release -Dgdk-pixbuf=disabled \
	-Dman-pages=disabled -Dwerror=true
ninja -C "$work/swaybg-build" -j"$JOBS"
copy "$work/swaybg-build/swaybg" usr/libexec/labwc/swaybg

wayland-scanner client-header "$work/swaybg-1.2.2/wlr-layer-shell-unstable-v1.xml" \
	"$work/layer-shell.h"
wayland-scanner private-code "$work/swaybg-1.2.2/wlr-layer-shell-unstable-v1.xml" \
	"$work/layer-shell.c"
wayland-scanner private-code "$work/sdk/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml" \
	"$work/xdg-shell.c"
wayland-scanner client-header "$wlroots/protocol/wlr-foreign-toplevel-management-unstable-v1.xml" \
	"$work/foreign-toplevel.h"
wayland-scanner private-code "$wlroots/protocol/wlr-foreign-toplevel-management-unstable-v1.xml" \
	"$work/foreign-toplevel.c"

panel_cflags=$(pkg-config --cflags wayland-client cairo)
panel_libs=$(pkg-config --libs wayland-client cairo)
SYNAPSE_LINK_MODE=dynamic "$CC" -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$work" -I"$SDK/include" $panel_cflags "$port/panel.c" \
	"$work/layer-shell.c" "$work/foreign-toplevel.c" "$work/xdg-shell.c" $panel_libs \
	-Wl,-rpath-link,"$alias_path/usr/lib" \
	-Wl,-rpath,/usr/lib/labwc/runtime:/usr/lib/weston/runtime:/usr/lib \
	-o "$work/synapse-panel"
copy "$work/synapse-panel" usr/libexec/labwc/synapse-panel

copy "$work/swaybg-1.2.2/LICENSE" usr/share/licenses/labwc/swaybg-LICENSE
copy "$port/Desktop-dark.png" usr/share/backgrounds/synapse/Desktop-dark.png
copy "$port/Desktop-light.png" usr/share/backgrounds/synapse/Desktop-light.png
copy "$port/autostart" etc/xdg/labwc/autostart
copy "$port/menu.xml" etc/xdg/labwc/menu.xml
copy "$port/rc.xml" etc/xdg/labwc/rc.xml
copy "$port/themerc" usr/share/themes/SynapseOS-Dark/openbox-3/themerc

themes="$work/openbox-theme-collections-8da2d26201f937e34d7fab71dc8e64d6ce4c94e0"
if [[ ! -d $themes ]]; then
	bsdtar -xf "$work/openbox-themes.tar.gz" -C "$work"
fi

for theme in Numix-Clone Arc-Clone Penumbra; do
	mkdir -p "$payload/usr/share/themes/$theme"
	cp -a "$themes/$theme/openbox-3" "$payload/usr/share/themes/$theme/"
done

copy "$themes/LICENSE" usr/share/licenses/labwc/openbox-theme-collections-LICENSE

runtime_deps usr/lib/labwc/runtime "$work/sdk/usr/lib" "$work/wlroots-stage/usr/lib" \
	"$BUILD_ROOT/mesa/llvm/root/usr/lib" "$BUILD_ROOT/mesa/payload/usr/lib" \
	"$BUILD_ROOT/openjdk/source/usr/lib"
rm -f "$payload/usr/lib/labwc/runtime/libLLVM.so.19.1" \
	"$payload/usr/lib/labwc/runtime/libgallium-24.3.4.so" \
	"$payload/usr/lib/labwc/runtime/libglapi.so.0" \
	"$payload/usr/lib/labwc/runtime/libGL.so.1"
