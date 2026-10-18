# recipe.sh: Собрать Arc JNI, звук, FreeType, SDL и модуль совместимости Java
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/$filename"
done < <(jq -r '.[] | [.filename,.url,.sha256] | @tsv' "$port/assets.json")

arc="$work/Arc-7445105cd2"
soloud="$work/soloud-eb83070e62e9edd3acc46cebbe345803c842620b"

if [[ ! -d $arc ]]; then
	bsdtar -xf "$work/arc.tar.gz" -C "$work"
fi

if [[ ! -d $soloud ]]; then
	bsdtar -xf "$work/soloud.tar.gz" -C "$work"
fi

if ! grep -q 'Synapse: determine EOF' "$soloud/src/core/soloud_file.cpp"; then
	patch --batch --fuzz=0 -d "$soloud" -p1 -i "$port/patches/soloud-buffering.patch"
fi

mkdir -p "$work/host-bin" "$work/jni-headers" "$work/jnigen"
printf '#!/bin/sh\nexec java com.sun.tools.javac.Main "$@"\n' >"$work/host-bin/javac"
chmod 755 "$work/host-bin/javac"
export PATH="$work/host-bin:$PATH"
jars="$work/jnigen/gdx.jar:$work/jnigen/loader.jar:$work/jnigen/parser.jar"
javac -cp "$work/jnigen/gdx.jar" -d "$work/jnigen" "$port/Generate.java"
bsdtar -xf "$work/jnigen/gdx.jar" -C "$work/jni-headers" --strip-components 6 \
	--include 'com/badlogic/gdx/jnigen/resources/headers/*'
common=(-I"$work/jni-headers" -I"$work/jni-headers/linux" -I"$SDK/include")
java -cp "$work/jnigen:$jars" Generate "$arc/arc-core/src" "$BUILD_ROOT/mindustry/downloads/Mindustry.jar" \
	"$work/arc-jni"
flags=("${common[@]}" -DWITH_OSS -DSOLOUD_MAX_VOICE_COUNT=100 -I"$work/arc-jni" -I"$work"
	-I"$soloud/include" -I"$soloud/src/audiosource/wav")
sources=("$work/arc-jni/"*.cpp "$port/soloud_synapse.cpp")

for directory in src/core src/audiosource/wav src/filter; do
	while IFS= read -r -d '' file; do
		sources+=("$file")
	done \
		< <(find "$soloud/$directory" -maxdepth 1 -type f \( -name '*.c' -o -name '*.cpp' \) -print0 | sort -z)
done

compile_objects "$work/core-objects" "${flags[@]}"
"$BUILD_ROOT/mesa/cxx" -shared -Wl,--no-undefined -Wl,-soname,libarc64.so "${objects[@]}" \
	-lm -lpthread -ldl -o "$work/libarc64.so"
ft="$work/freetype-2.14.3"

if [[ ! -d $ft ]]; then
	bsdtar -xf "$work/freetype.tar.gz" -C "$work"
fi

cat >"$ft/include/freetype/config/ftmodule.h" <<'MODULES'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
MODULES
java -cp "$work/jnigen:$jars" Generate "$arc/extensions/freetype/src" \
	"$BUILD_ROOT/mindustry/downloads/Mindustry.jar" "$work/freetype-jni"
sources=("$work/freetype-jni/"*.cpp)

for file in base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbbox base/ftglyph base/ftbitmap \
	base/ftstroke base/ftmm truetype/truetype sfnt/sfnt smooth/smooth autofit/autofit gzip/ftgzip psnames/psnames; do
	sources+=("$ft/src/$file.c")
done

compile_objects "$work/freetype-objects" "${common[@]}" -DFT2_BUILD_LIBRARY -I"$ft/include" -I"$work/freetype-jni"
"$BUILD_ROOT/mesa/cxx" -shared -Wl,--no-undefined -Wl,-soname,libarc-freetype64.so "${objects[@]}" \
	-lm -lpthread -ldl -o "$work/libarc-freetype64.so"
rm -rf "$work/sdl-jni"
java -cp "$work/jnigen:$jars" Generate "$arc/backends/backend-sdl/src" \
	"$BUILD_ROOT/mindustry/downloads/Mindustry.jar" "$work/sdl-jni"
# Области профилирования охватывают только экспортированные JNI-вызовы OpenGL
profile='s/(JNIEXPORT\s+\w+\s+JNICALL\s+'
profile+='Java_arc_backend_sdl_jni_SDLGL_(\w+)\s*\([^{}]*\)\s*\{)'
profile+='/$1\n synapse_profile::Scope profile("$2");/g'
perl -0777 -i -pe "$profile" \
	"$work/sdl-jni/arc_backend_sdl_jni_SDLGL.cpp"
fetch 'https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.tgz' \
	d4fc82893cfb00109578d0a1a2337fb8ca335b3ceccf97b97e5cc7f08e4353e1 "$work/glew/source.tar.gz"
glew="$work/glew/glew-2.2.0"

if [[ ! -d $glew ]]; then
	bsdtar -xf "$work/glew/source.tar.gz" -C "$work/glew"
fi

sed -i '/#  include <GL\/osmesa.h>/i #  define APIENTRY' "$glew/src/glew.c"
flags=("${common[@]}" -DGLEW_OSMESA -DGLEW_STATIC -DGLEW_NO_GLU -I"$work/sdl-jni" -I"$glew/include"
	-I"$BUILD_ROOT/mesa/mesa-24.3.4/include" -I"$BUILD_ROOT/sdl2/SDL2-2.30.11/include"
	-I"$BUILD_ROOT/sdl2/build/include")
sources=("$glew/src/glew.c" "$work/sdl-jni/"*.cpp)
sed -i "1i#include \"$port/gl-profile.h\"" "$work/sdl-jni/arc_backend_sdl_jni_SDLGL.cpp"
compile_objects "$work/sdl-objects" "${flags[@]}"
"$BUILD_ROOT/mesa/cxx" -shared -Wl,--no-undefined -Wl,-soname,libsdl-arc64.so "${objects[@]}" \
	"$BUILD_ROOT/sdl2/build/libSDL2-2.0.so.0" "$BUILD_ROOT/mesa/build/src/gallium/targets/osmesa/libOSMesa.so.8" \
	-o "$work/libsdl-arc64.so"
fetch 'https://raw.githubusercontent.com/Anuken/Mindustry/v160.7/core/src/mindustry/net/Net.java' \
	3256fd94ade9db179744bae7ee0615fad6dddb1d9d5ccca51e6b52382ad2abde "$work/net-upstream.java"
mkdir -p "$work/java-src/mindustry/net" "$work/java-src/arc/util" "$work/java-classes" "$work/agent/compat"
cp "$work/net-upstream.java" "$work/java-src/mindustry/net/Net.java"
patch --batch --fuzz=0 -d "$work/java-src" -p1 -i "$port/patches/net-executor.patch"

if ! grep -q synapse.mindustry.natives "$arc/arc-core/src/arc/util/SharedLibraryLoader.java"; then
	patch --batch --fuzz=0 -d "$arc" -p1 -i "$port/patches/arc-loader.patch"
fi

cp "$arc/arc-core/src/arc/util/SharedLibraryLoader.java" "$work/java-src/arc/util/"
classes="$work/java-classes"
# В upstream Net используется неоднородная таблица обработчиков с raw Cons
javac -source 17 -target 17 -Xlint:all,-options,-rawtypes -Werror \
	-cp "$BUILD_ROOT/mindustry/downloads/Mindustry.jar" -d "$classes" "$work/java-src/mindustry/net/Net.java"
javac -source 17 -target 17 -Xlint:all,-options -Werror -cp "$BUILD_ROOT/mindustry/downloads/Mindustry.jar" \
	-d "$classes" "$work/java-src/arc/util/SharedLibraryLoader.java" "$port/Compatibility.java"
cp -r "$classes/synapse" "$work/agent/"
cp -r "$classes/arc" "$classes/mindustry" "$work/agent/compat/"
mkdir -p "$work/agent/META-INF/synapse/targets/mindustry/desktop" "$payload/usr/share/java"
touch "$work/agent/META-INF/synapse/targets/mindustry/desktop/DesktopLauncher.class"
printf 'Manifest-Version: 1.0\r\nPremain-Class: synapse.Compatibility\r\n\r\n' >"$work/agent/META-INF/MANIFEST.MF"
TZ=UTC find "$work/agent" -type f -exec touch -t 198001010000 {} +

(
	cd "$work/agent" &&
		find . -type f -printf '%P\n' | sort |
		TZ=UTC zip -X -q "$payload/usr/share/java/synapse-compat.jar" -@
)

mkdir -p "$work/natives/mindustry/net" "$payload/usr/share/games/mindustry"
cp "$work/libarc64.so" "$work/libarc-freetype64.so" "$work/libsdl-arc64.so" "$work/natives/"
cp "$classes/mindustry/net/"Net*.class "$work/natives/mindustry/net/"
TZ=UTC find "$work/natives" -type f -exec touch -t 198001010000 {} +

(
	cd "$work/natives" &&
		find . -type f -printf '%P\n' | sort |
		TZ=UTC zip -X -q "$payload/usr/share/games/mindustry/synapse-natives.jar" -@
)

copy "$work/java-src/mindustry/net/Net.java" usr/share/doc/mindustry-desktop/source/Net.java
copy "$ROOT/pkgs/mindustry/files/LICENSE" usr/share/licenses/mindustry-desktop/Mindustry-LICENSE
copy "$arc/LICENSE" usr/share/licenses/mindustry-desktop/Arc-LICENSE
copy "$glew/LICENSE.txt" usr/share/licenses/mindustry-desktop/GLEW-LICENSE
copy "$ft/docs/FTL.TXT" usr/share/licenses/mindustry-desktop/FreeType-FTL.TXT
copy "$soloud/LICENSE" usr/share/licenses/mindustry-desktop/SoLoud-LICENSE
copy "$SDK/docs/user/opengl.md" usr/share/doc/mindustry-desktop/README.md
