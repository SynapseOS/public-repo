# recipe.sh: Собрать Weston и Wayland с POSIX-адаптациями SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
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

fetch "$(jq -r .source "$spec")" "$(jq -r .sha256 "$spec")" "$work/weston.tar.xz"
source="$work/weston-13.0.3"

if [[ ! -d $source ]]; then
	bsdtar -xf "$work/weston.tar.xz" -C "$work"
fi

if ! grep -q synapse-backend.inc "$source/libweston/backend-headless/headless.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/synapse.patch"
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
alias="/tmp/synapse-public-weston-$(printf '%s' "$ROOT" | sha256sum | cut -c1-16)"

if [[ ! -e $alias ]]; then
	ln -s "$work/sdk" "$alias"
fi

[[ $(realpath "$alias") == $(realpath "$work/sdk") ]]
mkdir -p "$work/sdk/usr/bin"
ln -sfn "$SDK/apps/musl/lib/libc.so" "$work/sdk/usr/lib/libc.musl-x86_64.so.1"
ln -sfn "$(command -v wayland-scanner)" "$work/sdk/usr/bin/wayland-scanner"
export PKG_CONFIG_SYSROOT_DIR="$alias"
export PKG_CONFIG_LIBDIR="$alias/usr/lib/pkgconfig:$alias/usr/share/pkgconfig"

if [[ ! -f $work/wayland-build/build.ninja ]]; then
	meson setup "$work/wayland-build" "$wayland" --cross-file "$work/wayland-cross.ini" --prefix=/usr --libdir=lib \
		--buildtype=release -Dscanner=false -Dtests=false -Ddocumentation=false -Ddtd_validation=false -Dwerror=true
fi

ninja -C "$work/wayland-build" -j"$JOBS"

if [[ ! -f $work/build/build.ninja ]]; then
	meson setup "$work/build" "$source" --cross-file "$work/cross.ini" --prefix=/usr --libdir=lib \
		--buildtype=release -Dbackend-default=headless -Dbackend-drm=false -Dbackend-drm-screencast-vaapi=false \
		-Dbackend-pipewire=false -Dbackend-rdp=false -Dbackend-vnc=false -Dbackend-wayland=false \
		-Dbackend-x11=false -Drenderer-gl=false -Dxwayland=false -Dsystemd=false -Dremoting=false \
		-Dpipewire=false -Dscreenshare=false -Dshell-ivi=false -Dshell-kiosk=true -Dshell-fullscreen=false \
		-Dcolor-management-lcms=false -Dimage-jpeg=false -Dimage-webp=false -Dtools=terminal,info \
		-Ddemo-clients=false -Dsimple-clients=shm -Dwcap-decode=false -Dtest-junit-xml=false -Ddoc=false -Dwerror=true
fi

ninja -C "$work/build" -j"$JOBS"
meson install -C "$work/build" --destdir "$work/stage"

for directory in usr/lib usr/libexec usr/share/weston; do
	if [[ -d $work/stage/$directory ]]; then
		mkdir -p "$payload/$(dirname "$directory")"
		cp -rL "$work/stage/$directory" "$payload/$(dirname "$directory")/"
	fi
done

for file in "$work/stage/usr/bin/"*; do
	copy "$file" "usr/libexec/weston/$(basename "$file")"
done

runtime_deps usr/lib/weston/runtime "$work/wayland-build/src" "$work/wayland-build/cursor" "$work/sdk/usr/lib"
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
copy "$port/weston.ini" etc/xdg/weston/weston.ini
"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none "$port/launch.c" -o "$work/weston-launch"

for file in weston weston-terminal weston-simple-shm; do
	copy "$work/weston-launch" "bin/$file"
done

copy "$source/COPYING" usr/share/licenses/weston/COPYING

copy "$port/README.md" usr/share/doc/weston/README.md
