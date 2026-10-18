/*
    tun.h: Пакетный интерфейс туннелей
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: пользовательский ABI SynapseOS

    Описание: Пакетный интерфейс туннелей
    Подсистема: сеть
    Первое изменение: 12.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYN_TUN_H
#define SYN_TUN_H

#include <stdint.h>

#define SYN_TUN_VERSION 1u
#define SYN_TUN_CONFIG 0x53545501u
#define SYN_TUN_STATS 0x53545502u
#define SYN_TUN_MAX_PACKET 1500u

struct syn_tun_config {
	uint32_t version, address, netmask, mtu;
	char name[16];
};

struct syn_tun_stats {
	uint64_t transmitted, received, dropped;
};

#endif
