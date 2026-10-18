# recipe.sh: Собрать захват кадров поверх опубликованного пользовательского ABI
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
cp "$port/pcap-synapse.c" "$source/pcap-null.c"
sed -i '0,/pcap_lookupnet(const char \*device,/s//pcap_generic_unused_lookupnet(const char *device,/' "$source/pcap.c"
export CFLAGS="-O2 -D__synapse__ -U__linux__ -I$SDK/include" CPPFLAGS="-I$SDK/include" PKG_CONFIG=false

(
	cd "$source" &&
		./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
			--disable-shared --disable-remote --with-pcap=null --without-libnl --disable-dbus --disable-bluetooth --disable-usb
)

make -C "$source" -j"$JOBS"
copy "$source/libpcap.a" usr/lib/libpcap.a

for file in "$source/pcap/"*.h; do
	copy "$file" "usr/include/pcap/$(basename "$file")"
done

copy "$source/pcap.h" usr/include/pcap.h
copy "$source/LICENSE" usr/share/licenses/libpcap/LICENSE
copy "$SDK/include/dev/packet_capture.h" usr/include/synapse/packet_capture.h
