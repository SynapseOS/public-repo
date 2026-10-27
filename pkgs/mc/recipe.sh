# recipe.sh: Собрать Midnight Commander и его данные без VFS и фоновых операций
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 25.10.2026
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
copy "$source/src/mc" bin/mc.real

cat >"$work/mc-launch.c" <<'C'
/*
    mc-launch.c: Запуск Midnight Commander в совместимом режиме терминала
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Настройка терминала и прямая замена процесса на mc.real
    Подсистема: пользовательские программы
    Первое изменение: 25.10.2026
    Последнее изменение: 25.10.2026
    Доступ: общедоступный
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Передать аргументы mc.real без промежуточного процесса оболочки
int main(int argc, char **argv) {
    if (setenv("TERM", "linux", 1) != 0) {
        perror("mc: TERM");
        return 1;
    }

    char **arguments = calloc((size_t)argc + 2, sizeof(*arguments));
    if (arguments == NULL) {
        perror("mc: arguments");
        return 1;
    }

    arguments[0] = "/bin/mc.real";
    arguments[1] = "-a";
    for (int index = 1; index < argc; index++) {
        arguments[index + 1] = argv[index];
    }

    execv(arguments[0], arguments);
    perror("mc: execv");
    free(arguments);
    return 1;
}
C

"$CC" -O2 -std=c11 -Wall -Wextra -Werror -Wl,--build-id=none "$work/mc-launch.c" -o "$payload/bin/mc"

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
