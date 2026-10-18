# recipe.sh: Собрать Screen со статической библиотекой ncurses
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
ncurses="$SDK/apps/apps/common/ncurses/install"
export CFLAGS='-O2 -D__synapse__ -ffunction-sections -fdata-sections' LDFLAGS='-static -Wl,--gc-sections'
export LIBS="-L$ncurses/lib -lncursesw" CPPFLAGS="-I$ncurses/include" PKG_CONFIG=false

(
	cd "$source" &&
		./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
			--disable-pam --disable-utmp --disable-telnet
)

make -C "$source" -j"$JOBS"
copy "$source/screen" bin/screen
copy "$source/COPYING" usr/share/licenses/screen/COPYING

for file in "$source/utf8encodings/"??; do
	copy "$file" "usr/share/screen/utf8encodings/$(basename "$file")"
done

copy "$port/screenrc" etc/screenrc
copy "$SDK/apps/apps/common/ncurses/upstream/COPYING" usr/share/licenses/screen/ncurses-COPYING
