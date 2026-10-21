# recipe.sh: Собрать консольный Vim со статической ncurses
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 21.10.2026, последнее изменение: 21.10.2026
# Статус доступа: общедоступный

prepare
ncurses="$SDK/apps/apps/common/ncurses/install"
export CFLAGS='-O2 -D__synapse__ -ffunction-sections -fdata-sections'
export CPPFLAGS="-I$ncurses/include -I$ncurses/include/ncursesw"
export LDFLAGS="-static -Wl,--gc-sections -L$ncurses/lib"
export LIBS="-L$ncurses/lib -lncursesw"
export PKG_CONFIG=false

(
    cd "$source/src" &&
        ./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
            --with-features=normal --enable-gui=no --without-x --disable-nls \
            --disable-gpm --disable-selinux --disable-xattr --with-tlib=ncursesw
)

make -C "$source/src" -j"$JOBS" vim
copy "$source/src/vim" bin/vim
copy "$source/LICENSE" usr/share/licenses/vim/LICENSE

for file in "$source/runtime/"*.vim; do
    copy "$file" "usr/share/vim/vim92/$(basename "$file")"
done

for directory in autoload colors doc; do
    while IFS= read -r -d '' file; do
        copy "$file" "usr/share/vim/vim92/${file#"$source/runtime/"}"
    done < <(find "$source/runtime/$directory" -type f -print0)
done

for name in c cpp html json markdown nosyntax python sh syncolor synload syntax vim; do
    copy "$source/runtime/syntax/$name.vim" "usr/share/vim/vim92/syntax/$name.vim"
done

copy "$SDK/apps/apps/common/ncurses/upstream/COPYING" usr/share/licenses/vim/ncurses-COPYING
