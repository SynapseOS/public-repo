# recipe.sh: собрать libseccomp для зависимостей Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: конструктор фильтров для будущего системного вызова seccomp
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare

fetch "$(jq -er .gperf_source "$spec")" \
	"$(jq -er .gperf_sha256 "$spec")" "$work/gperf.deb"
fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"

host_tools=$(mktemp -d "${TMPDIR:-/tmp}/synapse-libseccomp-tools.XXXXXX")
trap 'rm -rf -- "$host_tools"' EXIT
ar p "$work/gperf.deb" data.tar.xz | tar -xJf - -C "$host_tools"
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$host_tools"
ln -s "$host_tools/usr/include/x86_64-linux-gnu/asm" "$host_tools/usr/include/asm"
[[ -x $host_tools/usr/bin/gperf ]]

(
	cd "$source"
	CC="$CC" GPERF="$host_tools/usr/bin/gperf" \
	CFLAGS='-O2 -Werror -fPIC' CPPFLAGS="-D__synapse__ -I$host_tools/usr/include" \
		./configure --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --disable-shared --enable-static --disable-python

	make -C src -j"$JOBS" libseccomp.la
)

copy "$source/src/.libs/libseccomp.a" usr/lib/libseccomp.a
copy "$source/include/seccomp.h" usr/include/seccomp.h
copy "$source/include/seccomp-syscalls.h" usr/include/seccomp-syscalls.h
copy "$source/LICENSE" usr/share/licenses/libseccomp/LICENSE

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libseccomp.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libseccomp
Description: Seccomp filter builder for SynapseOS
Version: 2.6.1
Libs: -L${libdir} -lseccomp
Cflags: -I${includedir}
PC
