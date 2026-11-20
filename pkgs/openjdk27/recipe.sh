# recipe.sh: упаковка консольного JDK из Corretto для musl
# Автор: Елчинян Арен Артурович, 2026, x86_64, ABI Linux и musl
# Подсистема: сборка пакетов
# Первое изменение: 20.11.2026, последнее изменение: 20.11.2026
# Статус доступа: общедоступный

java_home=usr/lib/jvm/java-27-openjdk
jdk="$payload/$java_home"
archive="$work/downloads/jdk.tar.gz"
zlib="$work/downloads/zlib.apk"
fetch "$(jq -er .source "$spec")" "$(jq -er .sha256 "$spec")" "$archive"
fetch "$(jq -er .zlib.url "$spec")" "$(jq -er .zlib.sha256 "$spec")" "$zlib"
mkdir -p "$jdk" "$work/zlib"

# Исключаем инструменты X11, ALSA, профилировщик на glibc
bsdtar --strip-components=1 -xf "$archive" -C "$jdk" \
    --exclude '*/bin/asprof' --exclude '*/bin/jconsole' --exclude '*/bin/jpackage' \
    --exclude '*/lib/libasyncProfiler.so' --exclude '*/lib/libawt_xawt.so' --exclude '*/lib/libjawt.so' \
    --exclude '*/lib/libjsound.so' --exclude '*/lib/libsplashscreen.so' \
    --exclude '*/lib/src.zip' --exclude '*/lib/server/classes*.jsa' --exclude '*/man'
bsdtar --options=read_concatenated_archives -xf "$zlib" -C "$work/zlib" --include 'usr/lib/libz.so*'

# Проверяем зависимости всех инструментов и библиотек JDK и добавляем нужные из закреплённого APK
runtime_deps "$java_home/lib" "$work/zlib/usr/lib"

# Включаем zlib в java.base, чтобы образы jlink запускались с системной musl
# JMOD состоит из четырёх байт заголовка и архива ZIP
(
    stage=$(mktemp -d "$work/jlink.XXXXXX")
    trap 'rm -r -- "$stage"' EXIT
    cd "$stage"
    mkdir lib
    cp "$jdk/lib/libz.so.1" lib/
    touch -d @315619200 lib/libz.so.1
    base="$jdk/jmods/java.base.jmod"
    tail -c +5 "$base" > base.zip
    TZ=UTC zip -Xq base.zip lib/libz.so.1
    head -c 4 "$base" > base.jmod
    cat base.zip >> base.jmod
    mv base.jmod "$base"

    # Сжатый образ классов оставляет в пакете место для JMOD в пределах 256 МиБ
    "$jdk/bin/jlink" -J-XX:ActiveProcessorCount="$JOBS" -J-Xmx1g -J-Djava.io.tmpdir="$TMPDIR" \
        --module-path "$jdk/jmods" --add-modules ALL-MODULE-PATH --compress zip-6 \
        --no-man-pages --output image
    mv image/lib/modules "$jdk/lib/modules"
)

for tool in java javac jar jlink; do
    "$CC" -O2 -Wall -Wextra -Werror "-DJAVA_TOOL=\"/$java_home/bin/$tool\"" "$port/launch.c" -o "$payload/bin/${tool}27"
done

copy "$port/help.md" usr/share/doc/openjdk27/README.md
