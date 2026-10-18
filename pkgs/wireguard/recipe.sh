# recipe.sh: Собрать пользовательский адаптер WireGuard, реализация TUN остаётся в ядре
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
sed -i '/#include "lwip\/netif.h"/,/#include "lwip\/arch.h"/c\struct netif;\
struct udp_pcb;\
typedef uint32_t ip_addr_t;\
typedef uint16_t u16_t;' "$source/src/wireguard.h"
sed -i 's/uint32_t diff;/uint64_t diff;/' "$source/src/wireguard.c"
"$CC" -O2 -std=gnu11 -fno-strict-aliasing -I"$source/src" -I"$SDK/include" \
	"$port/wireguard.c" "$source/src/wireguard.c" "$source/src/crypto.c" "$source/src/crypto/refc/"*.c \
	-o "$payload/bin/wireguard"
copy "$source/LICENSE" usr/share/licenses/wireguard/LICENSE
copy "$source/src/crypto/refc/x25519-license.txt" usr/share/licenses/wireguard/x25519-license.txt
copy "$port/wireguard.conf.example" etc/wireguard.conf.example
