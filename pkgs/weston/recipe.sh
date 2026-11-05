# recipe.sh: Собрать Weston и Wayland с POSIX-адаптациями SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

mkdir -p "$work/sdk"

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/downloads/$filename"
	bsdtar --options=read_concatenated_archives -tf "$work/downloads/$filename" >"$work/sdk-members"
	filters=()

	for prefix in usr/include/ usr/lib/ usr/share/ etc/fonts/; do
		if grep -E "^$prefix" "$work/sdk-members" >/dev/null; then
			filters+=(--include "$prefix*")
		fi
	done

	if ((${#filters[@]})); then
		bsdtar --options=read_concatenated_archives -xf "$work/downloads/$filename" \
			-C "$work/sdk" "${filters[@]}"
	fi
done < <(jq -r '.[] | [.filename,.url,.sha256] | @tsv' "$port/dependencies.json")

mesa_stage="$BUILD_ROOT/mesa/stage/usr"
for directory in EGL GLES2 GLES3 KHR; do
	[[ -d $mesa_stage/include/$directory ]]
	cp -a "$mesa_stage/include/$directory" "$work/sdk/usr/include/"
done

for file in libEGL.so libEGL.so.1 libEGL.so.1.0.0 \
	libGLESv2.so libGLESv2.so.2 libGLESv2.so.2.0.0; do
	[[ -e $mesa_stage/lib/$file ]]
	cp -a "$mesa_stage/lib/$file" "$work/sdk/usr/lib/"
done

for file in egl.pc glesv2.pc; do
	[[ -f $mesa_stage/lib/pkgconfig/$file ]]
	cp "$mesa_stage/lib/pkgconfig/$file" "$work/sdk/usr/lib/pkgconfig/"
done

for file in xfixes.pc xxf86vm.pc; do
	cp "$BUILD_ROOT/mesa/wayland-sdk/usr/lib/pkgconfig/$file" \
		"$work/sdk/usr/lib/pkgconfig/"
done

for file in "$BUILD_ROOT/mesa/wayland-sdk/usr/lib"/libXfixes.so* \
	"$BUILD_ROOT/mesa/wayland-sdk/usr/lib"/libXxf86vm.so*; do
	cp -a "$file" "$work/sdk/usr/lib/"
done

fetch "$(jq -r .source "$spec")" "$(jq -r .sha256 "$spec")" "$work/weston.tar.xz"
source="$work/weston-13.0.3"

if [[ ! -d $source ]]; then
	bsdtar -xf "$work/weston.tar.xz" -C "$work"
fi

if ! grep -q synapse-backend.inc "$source/libweston/backend-headless/headless.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/synapse.patch"
fi

if ! grep -q 'KEY_S, MODIFIER_CTRL | MODIFIER_ALT' "$source/compositor/weston-screenshooter.c"; then
	sed -n '/^--- a\/compositor\/weston-screenshooter.c/,$p' "$port/patches/synapse.patch" |
		patch --batch --fuzz=0 -d "$source" -p1
fi

if ! grep -q 'KEY_SYSRQ, 0' "$source/compositor/weston-screenshooter.c"; then
	sed -i '/weston_compositor_add_key_binding(ec, KEY_R, MODIFIER_SUPER,/i\	weston_compositor_add_key_binding(ec, KEY_SYSRQ, 0,\n\t\t\t\t\t  screenshooter_binding, shooter);' \
		"$source/compositor/weston-screenshooter.c"
fi

if ! grep -q 'strdup("/bin/weston-screenshooter")' "$source/compositor/weston-screenshooter.c"; then
	sed -i 's|wet_get_bindir_path("weston-screenshooter")|strdup("/bin/weston-screenshooter")|' \
		"$source/compositor/weston-screenshooter.c"
fi

if ! grep -q '#include <string.h>' "$source/compositor/weston-screenshooter.c"; then
	sed -i '/#include <stdint.h>/a #include <string.h>' "$source/compositor/weston-screenshooter.c"
fi

if ! grep -q synapse_menu_binding "$source/desktop-shell/shell.c"; then
	sed -n '/^--- a\/desktop-shell\/shell.c/,$p' "$port/patches/synapse.patch" |
		patch --batch --fuzz=0 -d "$source" -p1
fi

if ! grep -q desktop_shell_list_minimized "$source/desktop-shell/shell.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/minimized-taskbar.patch"
fi

if ! grep -q 'Загрузить значок панели и уменьшить крупное изображение' "$source/clients/desktop-shell.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/launcher-icon-size.patch"
fi

if ! grep -q 'shell_surface_publish_task' "$source/desktop-shell/shell.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/window-taskbar.patch"
fi

if ! grep -q 'window_add_widget(terminal->window, terminal)' "$source/clients/terminal.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/terminal-server-decorations.patch"
fi

cp "$port/synapse-backend.inc" "$source/libweston/backend-headless/"
cp "$port/client-timers.inc" "$port/wayland-timers.inc" "$source/clients/"
fetch "$(jq -r .wayland_source "$spec")" "$(jq -r .wayland_sha256 "$spec")" "$work/wayland.tar.xz"
wayland="$work/wayland-1.23.1"

if [[ ! -d $wayland ]]; then
	bsdtar -xf "$work/wayland.tar.xz" -C "$work"
fi

if ! grep -q wayland-timers.inc "$wayland/src/event-loop.c"; then
	patch --batch --fuzz=0 -d "$wayland" -p1 -i "$port/patches/wayland-posix-events.patch"
fi

cp "$port/wayland-timers.inc" "$port/wayland-signals.inc" "$wayland/src/"
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
c_link_args = ['-Wl,-rpath-link,$work/sdk/usr/lib']
CROSS
sed "s/'-D_GNU_SOURCE', //" "$work/cross.ini" >"$work/wayland-cross.ini"
alias="$work/sdk"
mkdir -p "$work/sdk/usr/bin"
ln -sfn "$SDK/apps/musl/lib/libc.so" "$work/sdk/usr/lib/libc.musl-x86_64.so.1"
ln -sfn "$(command -v wayland-scanner)" "$work/sdk/usr/bin/wayland-scanner"
export PKG_CONFIG_SYSROOT_DIR="$alias"
export PKG_CONFIG_LIBDIR="$alias/usr/lib/pkgconfig:$alias/usr/share/pkgconfig"

meson_wipe=()
if [[ -f $work/wayland-build/build.ninja ]] &&
	grep -q '/tmp/synapse-public-weston-' "$work/wayland-build/build.ninja"; then
	meson_wipe=(--wipe)
fi

if [[ ! -f $work/wayland-build/build.ninja ]] || ((${#meson_wipe[@]})); then
	meson setup "$work/wayland-build" "$wayland" "${meson_wipe[@]}" \
		--cross-file "$work/wayland-cross.ini" --prefix=/usr --libdir=lib \
		--buildtype=release -Dscanner=false -Dtests=false -Ddocumentation=false -Ddtd_validation=false -Dwerror=true
fi

ninja -C "$work/wayland-build" -j"$JOBS"

meson_wipe=()
if [[ -f $work/build/build.ninja ]] &&
	grep -q '/tmp/synapse-public-weston-' "$work/build/build.ninja"; then
	meson_wipe=(--wipe)
fi

if [[ ! -f $work/build/build.ninja ]] || ((${#meson_wipe[@]})); then
	meson setup "$work/build" "$source" "${meson_wipe[@]}" \
		--cross-file "$work/cross.ini" --prefix=/usr --libdir=lib \
		--buildtype=release -Dbackend-default=headless -Dbackend-drm=false -Dbackend-drm-screencast-vaapi=false \
		-Dbackend-pipewire=false -Dbackend-rdp=false -Dbackend-vnc=false -Dbackend-wayland=false \
		-Dbackend-x11=false -Drenderer-gl=true -Dxwayland=false -Dsystemd=false -Dremoting=false \
		-Dpipewire=false -Dscreenshare=false -Dshell-ivi=false -Dshell-kiosk=true -Dshell-fullscreen=false \
		-Dcolor-management-lcms=false -Dimage-jpeg=false -Dimage-webp=false -Dtools=terminal,info \
		-Ddemo-clients=false -Dsimple-clients=shm -Dwcap-decode=false -Dtest-junit-xml=false -Ddoc=false -Dwerror=true
else
	meson configure "$work/build" -Drenderer-gl=true
fi

ninja -C "$work/build" -j"$JOBS"
meson install -C "$work/build" --destdir "$work/stage"

fetch "$(jq -r .libdecor_source "$spec")" "$(jq -r .libdecor_sha256 "$spec")" "$work/libdecor.tar.gz"
decor="$work/libdecor-0.2.3"

if [[ ! -d $decor ]]; then
	bsdtar -xf "$work/libdecor.tar.gz" -C "$work"
fi

if ! grep -q 'int component_x = 0;' "$decor/src/plugins/cairo/libdecor-cairo.c"; then
	patch --batch --fuzz=0 -d "$decor" -p1 -i "$port/patches/libdecor-init-size.patch"
fi

meson_wipe=()
if [[ -f $work/libdecor-build/build.ninja ]] &&
	grep -q '/tmp/synapse-public-weston-' "$work/libdecor-build/build.ninja"; then
	meson_wipe=(--wipe)
fi

if [[ ! -f $work/libdecor-build/build.ninja ]] || ((${#meson_wipe[@]})); then
	meson setup "$work/libdecor-build" "$decor" "${meson_wipe[@]}" --cross-file "$work/cross.ini" \
		--prefix=/usr --libdir=lib --buildtype=release -Ddbus=disabled -Dgtk=disabled \
		-Ddemo=false -Dwerror=true
fi

ninja -C "$work/libdecor-build" -j"$JOBS"
meson install -C "$work/libdecor-build" --destdir "$work/libdecor-stage"

for directory in usr/lib usr/libexec usr/share/weston; do
	if [[ -d $work/stage/$directory ]]; then
		mkdir -p "$payload/$(dirname "$directory")"
		cp -rL "$work/stage/$directory" "$payload/$(dirname "$directory")/"
	fi
done

copy "$work/libdecor-stage/usr/lib/libdecor-0.so.0" usr/lib/libdecor-0.so.0
copy "$work/libdecor-stage/usr/lib/libdecor/plugins-1/libdecor-cairo.so" \
	usr/lib/libdecor/plugins-1/libdecor-cairo.so
copy "$decor/LICENSE" usr/share/licenses/libdecor/LICENSE

for file in "$work/stage/usr/bin/"*; do
	copy "$file" "usr/libexec/weston/$(basename "$file")"
done

gl_plugin="$payload/usr/lib/libweston-13/gl-renderer.so"
[[ -f $gl_plugin ]]
mv "$gl_plugin" "$work/gl-renderer.so"
runtime_deps usr/lib/weston/runtime "$work/wayland-build/src" "$work/wayland-build/cursor" "$work/sdk/usr/lib"
mv "$work/gl-renderer.so" "$gl_plugin"
parent_payload=$payload
payload="$work/client-payload"
mkdir -p "$payload"

for file in libwayland-client.so.0 libwayland-cursor.so.0 libxkbcommon.so.0; do
	copy "$parent_payload/usr/lib/weston/runtime/$file" "usr/lib/$file"
done

for file in wayland-client.h wayland-client-core.h wayland-client-protocol.h \
	wayland-util.h wayland-version.h wayland-cursor.h; do
	copy "$work/sdk/usr/include/$file" "usr/include/$file"
done

mkdir -p "$payload/usr/include"
cp -rL "$work/sdk/usr/include/xkbcommon" "$payload/usr/include/"
copy "$wayland/COPYING" usr/share/licenses/wayland-client/COPYING
pack wayland-client "$payload" "$ROOT/pkgs/wayland-client/manifest.json"
payload="$work/xkb-payload"
mkdir -p "$payload/usr/share/X11" "$payload/usr/share/licenses/xkeyboard-config"
cp -rL "$work/sdk/usr/share/X11/xkb" "$payload/usr/share/X11/"
archive=$(jq -r '.[] | select(.name=="xkeyboard-config") | .filename' "$port/dependencies.json")
bsdtar --options=read_concatenated_archives -xOf "$work/downloads/$archive" .PKGINFO \
	>"$payload/usr/share/licenses/xkeyboard-config/APK-METADATA"
pack xkeyboard-config "$payload" "$ROOT/pkgs/xkeyboard-config/manifest.json"
payload=$parent_payload

for directory in usr/share/fonts usr/share/fontconfig usr/share/X11/locale etc/fonts usr/share/licenses; do
	if [[ -d $work/sdk/$directory ]]; then
		mkdir -p "$payload/$(dirname "$directory")"
		cp -rL "$work/sdk/$directory" "$payload/$(dirname "$directory")/"
	fi
done

mkdir -p "$payload/usr/share/icons/breeze_cursors"
cp -rL "$work/sdk/usr/share/icons/breeze_cursors/cursors" "$payload/usr/share/icons/breeze_cursors/"
copy "$work/sdk/usr/share/icons/breeze_cursors/index.theme" usr/share/icons/breeze_cursors/index.theme
copy "$port/mindustry-icon.png" usr/share/weston/mindustry-icon.png
copy "$port/synapse-logo.png" usr/share/weston/synapse-logo.png
copy "$port/settings-icon.png" usr/share/weston/settings-icon.png
copy "$port/Desktop.png" usr/share/backgrounds/synapse/Desktop.png
copy "$port/Desktop-pattern.png" usr/share/backgrounds/synapse/Desktop-pattern.png
copy "$port/Desktop-pattern-alt.png" usr/share/backgrounds/synapse/Desktop-pattern-alt.png
copy "$port/weston.ini" etc/xdg/weston/weston.ini
"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none "$port/launch.c" -o "$work/weston-launch"

for file in weston weston-terminal weston-simple-shm; do
	copy "$work/weston-launch" "bin/$file"
done

copy "$work/weston-launch" bin/weston-screenshooter

copy "$source/COPYING" usr/share/licenses/weston/COPYING

copy "$port/README.md" usr/share/doc/weston/README.md
