# recipe.sh: Собрать NetSurf с Duktape и SDL2 в окне Weston
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

fetch 'https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz' \
	4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b "$work/netsurf.tar.gz"
source="$work/netsurf-all-3.11"
rm -rf "$source"
bsdtar -xf "$work/netsurf.tar.gz" -C "$work"

for file in gcc14 curl-long-options curl-encodings default-surface javascript-defaults web-interfaces timer-order; do
	patch --batch --fuzz=0 -d "$source" -p1 -i "$port/patches/$file.patch"
done

javascript="$source/netsurf/content/handlers/javascript/duktape"
cp "$port/synapse-web.inc" "$javascript/"
cat "$port/synapse-web.js" >>"$javascript/polyfill.js"
cp "$port/sdl2-surface.c" "$source/libnsfb/src/surface/sdl.c"
prefix="$work/browser-prefix"
sdk="$BUILD_ROOT/weston/sdk/usr"
mkdir -p "$prefix/include" "$prefix/lib/pkgconfig" "$work/extra"

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/downloads/$filename"
	bsdtar --options=read_concatenated_archives -xf "$work/downloads/$filename" -C "$work/extra" --include 'usr/*'
done < <(jq -r '.[] | [.filename,.url,.sha256] | @tsv' "$port/dependencies.json")

for folder in include lib; do
	for origin in "$sdk/$folder/"* "$work/extra/usr/$folder/"*; do
		[[ -e $origin && $(basename "$origin") != pkgconfig ]] ||
			continue
		target="$prefix/$folder/$(basename "$origin")"

		if [[ -L $target ]]; then
			rm "$target"
		fi

		if [[ ! -e $target ]]; then
			ln -s "$origin" "$target"
		fi
	done
done

alias="/tmp/synapse-public-netsurf-$(printf '%s' "$ROOT" | sha256sum | cut -c1-16)"

if [[ ! -e $alias ]]; then
	ln -s "$prefix" "$alias"
fi

[[ $(realpath "$alias") == $(realpath "$prefix") ]]

for file in "$sdk/lib/pkgconfig/"*.pc "$work/extra/usr/lib/pkgconfig/"*.pc; do
	[[ -f $file ]] ||
		continue
	sed "s|^prefix=.*|prefix=$alias|" "$file" >"$prefix/lib/pkgconfig/$(basename "$file")"
done

curl="$BUILD_ROOT/curl/curl-8.22.0"
ssl="$BUILD_ROOT/openssl/openssl-3.5.9"
ln -sfn "$curl/lib/.libs/libcurl.a" "$prefix/lib/libcurl.a"
ln -sfn "$ssl/libssl.a" "$prefix/lib/libssl.a"
ln -sfn "$ssl/libcrypto.a" "$prefix/lib/libcrypto.a"
ln -sfn "$curl/include/curl" "$prefix/include/curl"
ln -sfn "$ssl/include/openssl" "$prefix/include/openssl"

for component in libcurl openssl; do
	version=3.5.9
	libs='-lssl -lcrypto'

	if [[ $component == libcurl ]]; then
		version=8.22.0
		libs="-lcurl $libs"
	fi

	cat >"$prefix/lib/pkgconfig/$component.pc" <<PC
prefix=$alias
Name: $component
Description: SynapseOS $component
Version: $version
Libs: -L\${prefix}/lib $libs
Cflags: -I\${prefix}/include
PC
done

sdl="$BUILD_ROOT/sdl2"
ln -sfn "$sdl/payload/usr/lib/libSDL2-2.0.so.0" "$prefix/lib/libSDL2.so"
cat >"$prefix/lib/pkgconfig/sdl.pc" <<PC
prefix=$alias
Name: sdl
Description: Synapse SDL2 surface
Version: 2.30.11
Libs: -L\${prefix}/lib -lSDL2
Cflags: -I$sdl/SDL2-2.30.11/include
PC
mkdir -p "$work/host-bin"
cat >"$work/host-bin/gperf" <<TOOL
#!/bin/sh
exec '$SDK/apps/musl/lib/libc.so' --library-path '$work/extra/usr/lib:$sdk/lib:$BUILD_ROOT/openjdk/source/usr/lib' \\
    '$work/extra/usr/bin/gperf' "\$@"
TOOL
chmod 755 "$work/host-bin/gperf"
export SYNAPSE_LINK_MODE=dynamic PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
export PATH="$work/host-bin:$PATH" CFLAGS="-O2 -isystem $(gcc -print-file-name=include) -I$alias/include -I$javascript"
export LDFLAGS="-no-pie -L$BUILD_ROOT/mesa/payload/usr/lib -L$alias/lib"
LDFLAGS+=" -Wl,-rpath-link,$BUILD_ROOT/mesa/payload/usr/lib -Wl,-rpath-link,$sdk/lib"
LDFLAGS+=' -Wl,-rpath,/usr/lib/netsurf/runtime,--disable-new-dtags -Wl,--allow-shlib-undefined'
common=(HOST=x86_64-linux-musl "CC=$CC" AR=ar "PREFIX=$alias" "NSSHARED=$source/buildsystem"
	VARIANT=release PKGCONFIG=pkg-config)

for component in buildsystem libwapcaplet libparserutils libcss libhubbub libdom libnsbmp libnsgif \
	librosprite libnsutils libutf8proc libnspsl libsvgtiny libnsfb; do
	options=()

	if [[ $component == libnsfb ]]; then
		options=(NSFB_SDL_AVAILABLE=yes NSFB_XCB_AVAILABLE=no NSFB_WLD_AVAILABLE=no NSFB_VNC_AVAILABLE=no)
	fi

	make -C "$source/$component" -j"$JOBS" install "${common[@]}" "${options[@]}"
done

env -u SYNAPSE_LINK_MODE -u CFLAGS -u LDFLAGS make -C "$source/nsgenbind" -j"$JOBS" install CC=cc \
	HOST=x86_64-linux-gnu "PREFIX=$alias" "NSSHARED=$source/buildsystem" VARIANT=release
export PATH="$alias/bin:$PATH"
cat >"$source/netsurf/Makefile.config" <<'CONFIG'
NETSURF_USE_DUKTAPE := YES
NETSURF_USE_NSLOG := NO
NETSURF_USE_LIBICONV_PLUG := YES
NETSURF_USE_JPEGXL := NO
NETSURF_USE_WEBP := NO
NETSURF_USE_NSSVG := YES
NETSURF_USE_OPENSSL := YES
NETSURF_FB_FONTLIB := freetype
NETSURF_FB_FONTPATH := /usr/share/fonts/dejavu
NETSURF_FB_RESPATH := /usr/share/netsurf
CONFIG
make -C "$source/netsurf" -j1 TARGET=framebuffer build/x86_64-linux-musl-framebuffer/duktape/binding.h "${common[@]}"
make -C "$source/netsurf" -j"$JOBS" TARGET=framebuffer "BUILD_LIBPNG_CFLAGS=-I$sdk/include/libpng16" \
	BUILD_LIBPNG_LDFLAGS=/usr/lib/x86_64-linux-gnu/libpng16.so.16 "${common[@]}"
copy "$source/netsurf/nsfb" bin/netsurf

for resource in resources frontends/framebuffer/res; do
	while IFS= read -r -d '' file; do
		[[ -f $file ]] ||
			continue
		[[ $file == *.ttf ]] &&
			continue
		copy "$file" "usr/share/netsurf/${file#"$source/netsurf/$resource/"}"
	done < <(find "$source/netsurf/$resource" \( -type f -o -type l \) -print0)
done

copy "$source/netsurf/COPYING" usr/share/licenses/netsurf/COPYING
awk '/\/\* LICENSE.txt \*\// {printing=1}
    printing {print}
    /\/\* AUTHORS.rst \*\// {authors=1}
    authors && /^\*\// {exit}' "$javascript/duktape.c" \
	>"$payload/usr/share/licenses/netsurf/Duktape-notices.txt"

for component in libwapcaplet libparserutils libcss libhubbub libdom libnsbmp libnsgif \
	librosprite libnsutils libutf8proc libnspsl libsvgtiny libnsfb; do
	for filename in COPYING LICENSE LICENCE LICENSE.md; do
		if [[ -f $source/$component/$filename ]]; then
			copy "$source/$component/$filename" "usr/share/licenses/netsurf/$component-$filename"
		fi
	done
done

copy "$curl/COPYING" usr/share/licenses/netsurf/curl-COPYING
copy "$ssl/LICENSE.txt" usr/share/licenses/netsurf/openssl-LICENSE.txt
runtime_deps usr/lib/netsurf/runtime "$prefix/lib"
