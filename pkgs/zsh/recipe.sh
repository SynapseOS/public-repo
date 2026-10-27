# recipe.sh: собрать Zsh со встроенными модулями для SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: собрать оболочку с редактором строки и установить функции дополнения
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

ncurses="$SDK/apps/apps/common/ncurses/install"
if [[ ! -f "$ncurses/lib/libncursesw.a" ]]; then
	echo 'Missing SDK ncurses library' >&2
	exit 1
fi

stage="$work/install"
rm -rf -- "$stage"
mkdir -p "$stage"

(
	cd "$source"
	CC="$CC" \
	CPPFLAGS="-D_GNU_SOURCE -D__synapse__ -I$ncurses/include -I$ncurses/include/ncursesw" \
	CFLAGS="-O2 -ffunction-sections -fdata-sections" \
	LDFLAGS="-static -Wl,--gc-sections -L$ncurses/lib" \
	LIBS="-lncursesw" \
		./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu \
			--prefix=/usr --bindir=/bin --disable-dynamic --enable-multibyte \
			--enable-libc-musl --with-term-lib=ncursesw

	make -j"$JOBS"
	make install.bin install.modules install.fns DESTDIR="$stage"
)

if [[ ! -f "$stage/bin/zsh" ]]; then
	echo 'Zsh binary missing from installation' >&2
	exit 1
fi

copy "$stage/bin/zsh" bin/zsh
strip --strip-debug "$payload/bin/zsh"
copy "$port/zshrc" etc/zshrc

while IFS= read -r -d '' file; do
	relative=${file#"$stage/"}
	copy "$file" "$relative"
done < <(find "$stage/usr/share/zsh" -type f -print0 | sort -z)

copy "$source/LICENCE" usr/share/licenses/zsh/LICENCE
