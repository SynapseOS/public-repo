# recipe.sh: Собрать Midnight Commander и его данные без VFS и фоновых операций
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
ncurses="$SDK/apps/apps/common/ncurses/install"
glib="$BUILD_ROOT/glib/payload/usr"
pcre="$BUILD_ROOT/pcre2/payload/usr"
export CFLAGS='-O2 -D__synapse__ -ffunction-sections -fdata-sections'
export CPPFLAGS="-I$ncurses/include -I$ncurses/include/ncursesw"
export LDFLAGS="-static -Wl,--gc-sections -L$ncurses/lib -L$pcre/lib"
export GLIB_CFLAGS="-I$glib/include/glib-2.0 -I$glib/lib/glib-2.0/include"
export GLIB_LIBS="-L$glib/lib -lglib-2.0 -lpcre2-8 -pthread -lm"
export PKG_CONFIG=pkg-config PKG_CONFIG_LIBDIR="$work/no-host-pkgconfig"

(
	cd "$source" &&
		./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr --sysconfdir=/etc \
			--with-screen=ncurses --without-x --without-gpm-mouse --without-subshell --disable-background --disable-vfs \
			--disable-nls --disable-aspell --enable-tests=no --disable-shared --enable-static
)

make -C "$source" -j"$JOBS"
make -C "$source" install DESTDIR="$work/install"
copy "$source/src/mc" bin/mc

for directory in etc/mc usr/share/mc; do
	while IFS= read -r -d '' file; do
		[[ -f $file ]] ||
			continue
		copy "$file" "${file#"$work/install/"}"
	done < <(find "$work/install/$directory" \( -type f -o -type l \) -print0)
done

copy "$source/COPYING" usr/share/licenses/mc/COPYING

for dependency in glib pcre2; do
	licenses="$BUILD_ROOT/$dependency/payload/usr/share/licenses/$dependency"

	while IFS= read -r -d '' file; do
		copy "$file" "usr/share/licenses/mc/$dependency/${file#"$licenses/"}"
	done < <(find "$licenses" -type f -print0)
done

copy "$SDK/apps/apps/common/ncurses/upstream/COPYING" usr/share/licenses/mc/ncurses-COPYING
