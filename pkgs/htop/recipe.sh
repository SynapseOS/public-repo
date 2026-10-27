# recipe.sh: Собрать htop с системным интерфейсом procfs
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

ncurses="$SDK/apps/apps/common/ncurses/install"
export CFLAGS='-O2 -D_GNU_SOURCE -D__synapse__'
export CPPFLAGS="-I$ncurses/include -I$ncurses/include/ncursesw"
export LDFLAGS="-static -L$ncurses/lib"
export PKG_CONFIG=false

(
	cd "$source"
	./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
		--enable-unicode --disable-capabilities --disable-sensors --disable-delayacct
)

make -C "$source" -j"$JOBS"

copy "$source/htop" bin/htop
copy "$source/COPYING" usr/share/licenses/htop/COPYING
copy "$SDK/apps/apps/common/ncurses/upstream/COPYING" usr/share/licenses/htop/ncurses-COPYING
