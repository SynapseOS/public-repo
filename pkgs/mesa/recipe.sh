# recipe.sh: Собрать Mesa OpenGL, EGL/Wayland и LLVM для musl
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 24.11.2026
# Статус доступа: общедоступный

fetch "$(jq -r .source "$spec")" "$(jq -r .sha256 "$spec")" "$work/mesa.tar.xz"
source="$work/mesa-24.3.4"

if [[ ! -d $source ]]; then
	bsdtar -xf "$work/mesa.tar.xz" -C "$work"
fi

if ! grep -q SYNAPSE_LLVMPIPE_WORKER "$source/src/gallium/drivers/llvmpipe/lp_rast.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/synapse-worker-metrics.patch"
fi

if ! grep -q 'errno == ENOSYS' "$source/src/util/anon_file.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/synapse-memfd-fallback.patch"
fi

mkdir -p "$work/llvm/root" "$work/linux-headers" "$work/cxx-headers" "$work/wayland-sdk"

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/wayland-sdk/downloads/$filename"
	bsdtar --options=read_concatenated_archives -xf "$work/wayland-sdk/downloads/$filename" \
		-C "$work/wayland-sdk" --include 'usr/*'
done < <(jq -r '.[] | select(.name == "wayland-dev" or .name == "wayland-libs-client" or
    .name == "wayland-libs-server" or .name == "wayland-libs-egl" or
    .name == "wayland-protocols" or .name == "libdrm" or .name == "libdrm-dev" or
    .name == "eudev-libs" or .name == "eudev-dev" or
    .name == "libffi" or .name == "libffi-dev" or .name == "libbsd" or .name == "libmd" or
    .name == "libexpat" or .name == "libxml2" or .name == "xz-libs" or .name == "zlib" or
    .name == "libx11" or .name == "libx11-dev" or .name == "libxau" or .name == "libxau-dev" or
    .name == "libxcb" or .name == "libxcb-dev" or .name == "libxdmcp" or .name == "libxdmcp-dev" or
    .name == "libxext" or .name == "libxext-dev" or
    .name == "libxrender" or .name == "libxrender-dev" or
    .name == "xcb-proto" or .name == "xorgproto") |
    [.filename,.url,.sha256] | @tsv' "$ROOT/pkgs/weston/dependencies.json")

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/wayland-sdk/downloads/$filename"
	bsdtar --options=read_concatenated_archives -xf "$work/wayland-sdk/downloads/$filename" \
		-C "$work/wayland-sdk" --include 'usr/*'
done < <(jq -r '.x11_assets[] | [.name,.url,.sha256] | @tsv' "$spec")

scanner="$work/wayland-sdk/usr/bin/wayland-scanner"
mv -f "$scanner" "$scanner.real"
cat >"$scanner" <<SCANNER
#!/bin/sh
# wayland-scanner: Запуск сборочного сканера с библиотеками musl
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка Mesa
# Первое изменение: 23.10.2026, последнее изменение: 23.10.2026
# Статус доступа: общедоступный
LD_LIBRARY_PATH='$work/wayland-sdk/usr/lib' exec '$scanner.real' "\$@"
SCANNER
chmod 755 "$scanner"

alias_path="/tmp/synapse-public-mesa-$(printf '%s' "$ROOT" | sha256sum | cut -c1-16)"
if [[ ! -e $alias_path ]]; then
	ln -s "$work/wayland-sdk" "$alias_path"
fi
[[ $(realpath "$alias_path") == $(realpath "$work/wayland-sdk") ]]
export PKG_CONFIG_SYSROOT_DIR="$alias_path"
export PKG_CONFIG_LIBDIR="$alias_path/usr/lib/pkgconfig:$alias_path/usr/share/pkgconfig"

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/llvm/downloads/$filename"
	bsdtar --options=read_concatenated_archives -xf "$work/llvm/downloads/$filename" \
		-C "$work/llvm/root" --include 'usr/*'
done < <(jq -r '.llvm_assets[] | [.name,.url,.sha256] | @tsv' "$spec")

fetch 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/linux-headers-6.14.2-r0.apk' \
	b0d7184f0e8d926961b82dff3d8a6a1f85db100dfc97e3fa1e6a25bbe9fd0f71 "$work/linux-headers.apk"
bsdtar --options=read_concatenated_archives -xf "$work/linux-headers.apk" -C "$work/linux-headers" --include 'usr/*'
fetch 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/libstdc++-dev-14.2.0-r6.apk' \
	13e2169b46a24f03c87caf91387312724b01951c222bd061d6d87c48903836a9 "$work/cxx-headers.apk"
bsdtar --options=read_concatenated_archives -xf "$work/cxx-headers.apk" -C "$work/cxx-headers" --include 'usr/*'
cp "$ROOT/build/cxx-shared.sh" "$work/cxx"
chmod 755 "$work/cxx"
cat >"$work/llvm-config" <<LLVM
#!/bin/sh
exec env LD_LIBRARY_PATH='$BUILD_ROOT/openjdk/source/usr/lib:$work/llvm/root/usr/lib' \\
    '$work/llvm/root/usr/lib/llvm19/bin/llvm-config' "\$@"
LLVM
chmod 755 "$work/llvm-config"
cat >"$work/cross.ini" <<CROSS
[binaries]
c = ['env', 'SYNAPSE_LINK_MODE=dynamic', '$CC']
cpp = ['env', 'SYNAPSE_LINK_MODE=dynamic', '$work/cxx']
ar = 'ar'
strip = 'strip'
llvm-config = '$work/llvm-config'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS

export LLVM_CONFIG="$work/llvm-config"
c_args="-D_GNU_SOURCE -isystem $(gcc -print-file-name=include)"
c_args+=" -idirafter $work/linux-headers/usr/include"
options=(-Dbuildtype=release -Dplatforms=x11,wayland
	-Dgallium-drivers=softpipe,llvmpipe,virgl -Dvulkan-drivers= -Dglx=dri
	-Degl=enabled -Dgbm=enabled -Dosmesa=true -Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=true
	-Dshared-glapi=enabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dvideo-codecs=
	-Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-nine=false -Dgles1=disabled -Dgles2=enabled
	-Dshader-cache=disabled -Dlibunwind=disabled -Dglvnd=disabled
	"-Dc_args=$c_args"
	"-Dcpp_args=-D_GNU_SOURCE -idirafter $work/linux-headers/usr/include")

if [[ -f $work/egl-build/build.ninja ]] &&
	! grep -Fq "$work/cxx" "$work/egl-build/build.ninja"; then
	meson setup "$work/egl-build" "$source" --wipe --cross-file "$work/cross.ini" \
		--prefix=/usr --libdir=lib "${options[@]}"
elif [[ -f $work/egl-build/build.ninja ]]; then
	meson configure "$work/egl-build" "${options[@]}"
else
	meson setup "$work/egl-build" "$source" --cross-file "$work/cross.ini" --prefix=/usr --libdir=lib "${options[@]}"
fi

ninja -C "$work/egl-build" -j"$JOBS"
meson install -C "$work/egl-build" --destdir "$work/stage"

for file in libOSMesa.so.8 libglapi.so.0 libgallium-24.3.4.so libEGL.so.1 libGLESv2.so.2 \
	libgbm.so.1; do
	copy "$work/stage/usr/lib/$file" "usr/lib/$file"
done

for file in swrast_dri.so kms_swrast_dri.so virtio_gpu_dri.so; do
	copy "$work/stage/usr/lib/dri/$file" "usr/lib/dri/$file"
done

copy "$work/stage/usr/lib/gbm/dri_gbm.so" usr/lib/gbm/dri_gbm.so

for file in libX11.so.6 libxcb-glx.so.0 libxcb.so.1 libX11-xcb.so.1 libXext.so.6 \
	libXxf86vm.so.1 libxcb-shm.so.0 libxcb-dri3.so.0 libxcb-present.so.0 \
	libxcb-xfixes.so.0; do
	copy "$work/wayland-sdk/usr/lib/$file" "usr/lib/$file"
done

for file in libdrm.so.2 libudev.so.1 libwayland-server.so.0 libwayland-egl.so.1; do
	copy "$work/wayland-sdk/usr/lib/$file" "usr/lib/$file"
done

for file in libgcc_s.so.1 libstdc++.so.6; do
	copy "$BUILD_ROOT/openjdk/source/usr/lib/$file" "usr/lib/$file"
done

for file in libLLVM.so.19.1 libffi.so.8 libzstd.so.1 libxml2.so.2 liblzma.so.5; do
	copy "$work/llvm/root/usr/lib/$file" "usr/lib/$file"
done

copy "$source/docs/license.rst" usr/share/licenses/mesa/license.rst
copy "$BUILD_ROOT/openjdk/source/usr/lib/libz.so.1" usr/lib/libz.so.1

for file in osmesa.h gl.h glext.h; do
	copy "$source/include/GL/$file" "usr/include/GL/$file"
done

for directory in EGL GLES2 GLES3 KHR; do
	mkdir -p "$payload/usr/include/$directory"
	cp -rL "$work/stage/usr/include/$directory/." "$payload/usr/include/$directory/"
done

copy "$SDK/docs/user/opengl.md" usr/share/doc/mesa/README.md

runtime_deps usr/lib "$work/wayland-sdk/usr/lib" "$work/llvm/root/usr/lib" \
	"$BUILD_ROOT/openjdk/source/usr/lib"
rm -f "$payload/usr/lib/libwayland-client.so.0"
