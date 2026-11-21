# SynapseOS, мультимедиа, 21.11.2026, сборка библиотек записи
set -eu
ff_arch=${TARGET_ARCH:-x86_64}
compiler_headers=$(gcc -print-file-name=include)
if [[ $ff_arch == aarch64 ]]; then
    compiler_headers=$("${ARM64_CC:-clang}" -print-resource-dir)/include
fi
./configure --cc="$CC" --enable-cross-compile --arch="$ff_arch" --target-os=linux --disable-autodetect --disable-programs --disable-doc --disable-network \
    --disable-everything --disable-avdevice --disable-avfilter --disable-swresample --enable-swscale \
    --enable-encoder=mpeg4,mjpeg,bmp,aac,png --enable-decoder=mjpeg,bmp,png --enable-muxer=matroska,mp4,avi \
    --enable-protocol=file --disable-debug --enable-small --disable-x86asm --disable-iconv \
    --enable-zlib --extra-ldflags="-L$BUILD_ROOT/zlib/payload/usr/lib" \
    --extra-cflags="-I$BUILD_ROOT/zlib/payload/usr/include -isystem $compiler_headers"
make -j"${JOBS:-4}"
