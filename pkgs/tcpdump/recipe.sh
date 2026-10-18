# recipe.sh: Собрать tcpdump только с библиотекой целевой системы
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
ln -sfn "$BUILD_ROOT/libpcap/libpcap-1.10.5" "$work/libpcap"
export CFLAGS='-O2 -D__synapse__ -U__linux__' LDFLAGS=-static PKG_CONFIG=false

(
	cd "$source" &&
		./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
			--without-crypto --without-cap-ng --disable-smb
)

make -C "$source" -j"$JOBS"
copy "$source/tcpdump" bin/tcpdump
copy "$source/LICENSE" usr/share/licenses/tcpdump/LICENSE
copy "$BUILD_ROOT/libpcap/libpcap-1.10.5/LICENSE" usr/share/licenses/tcpdump/libpcap-LICENSE
