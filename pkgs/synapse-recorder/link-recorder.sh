# SynapseOS, кодировщик и консольный интерфейс, 21.11.2026
set -eu
objects=()
compiler_headers=$(gcc -print-file-name=include)
strip_tool=strip
if [[ ${TARGET_ARCH:-x86_64} == aarch64 ]]; then
    compiler_headers=$("${ARM64_CC:-clang}" -print-resource-dir)/include
    strip_tool=${ARM64_STRIP:-llvm-strip}
fi
printf '%s\n' '#define MINIH264_IMPLEMENTATION' '#define H264E_MAX_THREADS 0' '#define H264E_SVC_API 0' \
    '#include <features.h>' '#undef __inline' '#include "minih264e.h"' > "$work/mini.c"
"$CC" -O2 -isystem "$compiler_headers" -c "$work/mini.c" -o "$work/mini.o"
objects+=("$work/mini.o")
for file in h264 control recorder tui io media image stills movie camera; do
    codec_flags=()
    # В FFmpeg 7.1 даже штатные FFCodec используют это поле при регистрации кодировщика.
    if [[ $file == h264 ]]; then codec_flags+=(-Wno-error=deprecated-declarations); fi
    "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror "${codec_flags[@]}" -I"$work" -I"$port" -I"$source" -I"$ROOT/sdk/include" \
        -c "$port/$file.c" -o "$work/$file.o"
    objects+=("$work/$file.o")
done
libs=()
for lib in avformat avcodec swscale avutil; do libs+=("$source/lib$lib/lib$lib.a"); done
"$CC" "${objects[@]}" -Wl,--start-group "${libs[@]}" -Wl,--end-group "$BUILD_ROOT/zlib/payload/usr/lib/libz.a" -lm -lpthread -o "$payload/bin/recorder"
"$strip_tool" "$payload/bin/recorder"

