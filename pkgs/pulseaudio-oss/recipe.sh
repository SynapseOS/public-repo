# recipe.sh: собрать PulseAudio с выводом через /dev/dsp
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: звуковой сервер и клиентские библиотеки для графических приложений
# Подсистема: звуковые пакеты
# Первое изменение: 27.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare
export SYNAPSE_LINK_MODE=dynamic

# pkg-config у Meson ожидает ASCII в путях зависимостей
ascii_root=$(mktemp -d "${TMPDIR:-/tmp}/synapse-pulse-work.XXXXXX")
rmdir "$ascii_root"
ln -s "$work" "$ascii_root"
trap 'rm -f -- "$ascii_root"' EXIT

fetch "$(jq -er .sndfile_source "$spec")" "$(jq -er .sndfile_sha256 "$spec")" \
	"$work/sndfile.tar.xz"
fetch "$(jq -er .libtool_source "$spec")" "$(jq -er .libtool_sha256 "$spec")" \
	"$work/libtool.tar.xz"
fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"

mkdir -p "$work/sndfile-source" "$work/libtool-source" "$work/headers" "$work/sysroot/usr"
bsdtar -xf "$work/sndfile.tar.xz" -C "$work/sndfile-source"
bsdtar -xf "$work/libtool.tar.xz" -C "$work/libtool-source"
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$work/headers"
ln -sfn "$work/headers/usr/include/x86_64-linux-gnu/asm" "$work/headers/usr/include/asm"

rm -rf -- "$work/sndfile-build"
cmake -S "$work/sndfile-source/libsndfile-1.2.2" -B "$work/sndfile-build" \
	-DCMAKE_C_COMPILER="$CC" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		-DCMAKE_INSTALL_PREFIX="$ascii_root/sysroot/usr" \
	-DCMAKE_C_FLAGS="-O2 -fPIC -isystem $(gcc -print-file-name=include)" \
	-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DENABLE_EXTERNAL_LIBS=OFF \
	-DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_SSE2=OFF -DENABLE_BOW_DOCS=OFF
cmake --build "$work/sndfile-build" -j"$JOBS"
cmake --install "$work/sndfile-build"

mkdir -p "$work/libltdl-build"
(
	cd "$work/libltdl-build"
	CC="$CC" CFLAGS='-O2 -fPIC' \
		"$work/libtool-source/libtool-2.5.4/libltdl/configure" \
		--host=x86_64-linux-musl --prefix=/usr --enable-ltdl-install \
		--disable-shared --enable-static
	make -j"$JOBS"
	make DESTDIR="$work/sysroot" install
)

# Сборка не должна подтянуть libcap хоста через автопоиск Meson
sed -i "s/^cap_dep = cc.find_library('cap', required : false)$/cap_dep = dependency('', required : false)/" \
	"$source/meson.build"

cat >"$work/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-O2', '-D__synapse__', '-I$ascii_root/sysroot/usr/include', '-I$ascii_root/headers/usr/include', '-isystem', '$(gcc -print-file-name=include)']
c_link_args = ['-L$ascii_root/sysroot/usr/lib', '-Wl,-rpath,/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$ascii_root/sysroot/usr/lib/pkgconfig"
rm -rf -- "$work/pulse-build" "$work/stage"
meson setup "$work/pulse-build" "$source" --cross-file "$work/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
	-Dauto_features=disabled -Ddaemon=true -Dclient=true -Doss-output=enabled \
	-Ddatabase=simple -Ddoxygen=false -Dman=false -Dtests=false \
	-Ddefault_library=shared

# Ядро пока не предоставляет futex PI, поэтому mutex использует обычный протокол
sed -i '/^#define HAVE_PTHREAD_PRIO_INHERIT 1$/d' "$work/pulse-build/config.h"
ninja -C "$work/pulse-build" -j"$JOBS"
DESTDIR="$work/stage" meson install -C "$work/pulse-build"

while IFS= read -r -d '' file; do
	path=${file#"$work/stage/"}
	copy "$file" "$path"
done < <(find "$work/stage/etc" "$work/stage/usr" \( -type f -o -type l \) -print0)

copy "$source/LICENSE" usr/share/licenses/pulseaudio-oss/LICENSE
copy "$work/sndfile-source/libsndfile-1.2.2/COPYING" \
	usr/share/licenses/pulseaudio-oss/libsndfile-COPYING
copy "$work/libtool-source/libtool-2.5.4/COPYING" \
	usr/share/licenses/pulseaudio-oss/libtool-COPYING
