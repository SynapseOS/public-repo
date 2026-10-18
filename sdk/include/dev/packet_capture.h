/*
    packet_capture.h: Захват сетевых кадров
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: пользовательский ABI SynapseOS

    Описание: Захват сетевых кадров
    Подсистема: сеть
    Первое изменение: 12.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYN_PACKET_CAPTURE_H
#define SYN_PACKET_CAPTURE_H

#include <stdint.h>

#define SYN_CAPTURE_VERSION 1
#define SYN_CAPTURE_BIND 0x53500001u
#define SYN_CAPTURE_INFO 0x53500002u
#define SYN_CAPTURE_STATS 0x53500003u
#define SYN_CAPTURE_IN 1u
#define SYN_CAPTURE_OUT 2u
#define SYN_CAPTURE_MAX_FRAME 9018u

struct syn_capture_bind {
	uint32_t version;
	int32_t ifindex;
	uint32_t snaplen, directions;
};

struct syn_capture_info {
	uint32_t version;
	int32_t ifindex;
	uint32_t count, flags;
	char name[16];
	uint8_t mac[6], reserved[2];
	uint32_t ipv4, netmask, gateway;
};

struct syn_capture_stats {
	uint64_t received, dropped, injected;
};

struct syn_capture_header {
	uint64_t timestamp_us;
	uint32_t wirelen, caplen, ifindex, direction;
};

#endif
