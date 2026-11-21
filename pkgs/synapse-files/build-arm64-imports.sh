#!/usr/bin/env bash
# Загружать Mesa при первом вызове EGL или GLES из ARM64 Files.
# Подсистема: графические пакеты SynapseOS, AArch64, MIT.
set -euo pipefail
archive=$(realpath "${1:?implib source archive}")
mkdir -p "${2:?work directory}" "${4:?package root}"
work=$(realpath "$2")
runtime=$(realpath "${3:?Qt runtime}")
stage=$(realpath "$4")
spec=$(dirname "$(realpath "$0")")/port.json
printf '%s  %s\n' "$(jq -er .implib_sha256 "$spec")" "$archive" | sha256sum --check --status
mkdir -p "$work/generator"
tar -xf "$archive" -C "$work/generator" --strip-components=1
export LC_ALL=C

# Сверить публичный ABI с библиотекой, поставляемой в образе.
exports() {
    readelf --dyn-syms --wide "$1" | awk '
        ($5=="GLOBAL" || $5=="WEAK") && $7!="UND" && ($6=="DEFAULT" || $6=="PROTECTED") {
            if ($4!="FUNC" || $8 !~ /^[a-zA-Z_][a-zA-Z_0-9]*$/) exit 1
            print $8
        }' | sort -u
}
for lib in libEGL.so.1 libGLESv2.so.2; do
    exports "$runtime/$lib" > "$work/$lib.exports"
    test -s "$work/$lib.exports"
    {
        echo '{ global:'
        sed 's/$/;/' "$work/$lib.exports"
        echo 'local: *; };'
    } > "$work/$lib.map"
    python3 "$work/generator/implib-gen.py" --target aarch64-linux-musl \
        --outdir "$work" --library-load-name "/usr/lib/qt6/runtime/$lib" "$runtime/$lib"
    "${CC:?ARM64 compiler}" -Os -fPIC -shared -DIMPLIB_EXPORT_SHIMS \
        "-Wl,-soname,$lib,--version-script=$work/$lib.map" \
        "$work/$lib.init.c" "$work/$lib.tramp.S" -o "$work/$lib"
    exports "$work/$lib" > "$work/$lib.actual"
    diff -u "$work/$lib.exports" "$work/$lib.actual"
    install -Dm755 "$work/$lib" "$stage/usr/lib/synapse-files/$lib"
done
install -Dm644 "$work/generator/LICENSE.txt" "$stage/usr/share/licenses/synapse-files/Implib-LICENSE"
