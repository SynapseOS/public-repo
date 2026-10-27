# recipe.sh: собрать статическую libe2p из e2fsprogs
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: свойства файловой системы для OSTree
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

mkdir -p "$work/build"
(
	cd "$work/build"
	CC="$CC" CFLAGS='-O2 -Werror -fPIC' CPPFLAGS='-D__synapse__' \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --disable-nls --disable-elf-shlibs

	make top-deps
	make -j"$JOBS" -C lib/e2p
)

copy "$work/build/lib/e2p/libe2p.a" usr/lib/libe2p.a
copy "$source/lib/e2p/e2p.h" usr/include/e2p/e2p.h
copy "$source/lib/ext2fs/ext2_fs.h" usr/include/ext2fs/ext2_fs.h
copy "$work/build/lib/ext2fs/ext2_types.h" usr/include/ext2fs/ext2_types.h
copy "$source/NOTICE" usr/share/licenses/libe2p/NOTICE

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/e2p.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: e2p
Description: Ext filesystem properties library
Version: 1.47.3
Libs: -L${libdir} -le2p
Cflags: -I${includedir}
PC
