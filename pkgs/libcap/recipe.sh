# recipe.sh: собрать статическую libcap из закреплённого исходника
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека capability для сборки Bubblewrap
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

make -C "$source/libcap" -j"$JOBS" \
	CC="$CC" BUILD_CC=gcc SHARED=no PTHREADS=no USE_GPERF=no \
	CFLAGS='-O2 -Werror -fPIC' libcap.a

copy "$source/libcap/libcap.a" usr/lib/libcap.a
copy "$source/libcap/include/sys/capability.h" usr/include/sys/capability.h
copy "$source/libcap/include/uapi/linux/capability.h" usr/include/linux/capability.h
copy "$source/License" usr/share/licenses/libcap/License

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libcap.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libcap
Description: Linux capability interface
Version: 2.78
Libs: -L${libdir} -lcap
Cflags: -I${includedir}
PC
