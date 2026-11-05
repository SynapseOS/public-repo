# recipe.sh: собрать статический эмулятор QEMU i386 из исходников
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: применить исправление устойчивых блокировок и упаковать эмулятор
# Подсистема: сборка пакетов
# Первое изменение: 29.10.2026
# Последнее изменение: 30.10.2026
# Доступ: общедоступный

prepare

mkdir -p "$work/qemu-build"
(
	cd "$work/qemu-build"
	export CC=gcc
	"$source/configure" --target-list=i386-linux-user --static --disable-system \
		--disable-docs --disable-tools --disable-guest-agent --disable-plugins \
		--disable-slirp --disable-capstone --disable-fdt --disable-gtk \
		--disable-sdl --disable-vnc --disable-debug-info
)

ninja -C "$work/qemu-build" -j"$JOBS" qemu-i386

readelf -h "$work/qemu-build/qemu-i386" | grep -q 'Machine:.*Advanced Micro Devices X86-64'
if readelf -d "$work/qemu-build/qemu-i386" | grep -q NEEDED; then
	echo 'qemu-i386 must not require host shared libraries' >&2
	exit 1
fi

copy "$work/qemu-build/qemu-i386" bin/qemu-i386
chmod 755 "$payload/bin/qemu-i386"
copy "$source/COPYING" usr/share/licenses/qemu-i386/COPYING
copy "$source/COPYING.LIB" usr/share/licenses/qemu-i386/COPYING.LIB
