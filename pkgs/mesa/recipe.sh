# recipe.sh: Собрать программный OpenGL и LLVM для musl
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

fetch "$(jq -r .source "$spec")" "$(jq -r .sha256 "$spec")" "$work/mesa.tar.xz"
source="$work/mesa-24.3.4"

if [[ ! -d $source ]]; then
	bsdtar -xf "$work/mesa.tar.xz" -C "$work"
fi

if ! grep -q SYNAPSE_LLVMPIPE_WORKER "$source/src/gallium/drivers/llvmpipe/lp_rast.c"; then
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/synapse-worker-metrics.patch"
fi

mkdir -p "$work/llvm/root" "$work/linux-headers" "$work/cxx-headers"

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
pkg-config = 'false'
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
options=(-Dbuildtype=release -Dplatforms= -Dgallium-drivers=softpipe,llvmpipe -Dvulkan-drivers= -Dglx=disabled
	-Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=true
	-Dshared-glapi=enabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dvideo-codecs=
	-Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-nine=false -Dgles1=disabled -Dgles2=disabled
	-Dshader-cache=disabled
	"-Dc_args=$c_args"
	"-Dcpp_args=-D_GNU_SOURCE -idirafter $work/linux-headers/usr/include")

if [[ -f $work/build/build.ninja ]]; then
	meson configure "$work/build" "${options[@]}"
else
	meson setup "$work/build" "$source" --cross-file "$work/cross.ini" --prefix=/usr --libdir=lib "${options[@]}"
fi

ninja -C "$work/build" -j"$JOBS"
meson install -C "$work/build" --destdir "$work/stage"

for file in libOSMesa.so.8 libglapi.so.0; do
	copy "$work/stage/usr/lib/$file" "usr/lib/$file"
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

copy "$SDK/docs/user/opengl.md" usr/share/doc/mesa/README.md
