/*
    firewall.h: Управление фильтром пакетов
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: пользовательский ABI SynapseOS

    Описание: Управление фильтром пакетов
    Подсистема: сеть
    Первое изменение: 12.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYN_FIREWALL_ABI_H
#define SYN_FIREWALL_ABI_H

#include <stdint.h>

#define SYN_FW_VERSION 1
#define SYN_FW_MAX_RULES 64
#define SYN_FW_GET 0x53460001u
#define SYN_FW_REPLACE 0x53460002u
#define SYN_FW_RESET_COUNTERS 0x53460003u
#define SYN_FW_IN 0
#define SYN_FW_OUT 1
#define SYN_FW_DROP 0
#define SYN_FW_ALLOW 1

// Целые числа передаются в порядке байтов хоста, адреса IP в сетевом порядке
struct syn_fw_rule {
	uint8_t direction, action, family, protocol;
	uint8_t src_prefix, dst_prefix, reserved[2];
	uint8_t src[16], dst[16];
	uint16_t sport_lo, sport_hi, dport_lo, dport_hi;
	uint64_t packets, bytes;
};

struct syn_fw_config {
	uint32_t version, enabled, count;
	uint8_t policy[2], reserved[2];
	struct syn_fw_rule rules[SYN_FW_MAX_RULES];
	uint64_t allowed[2], dropped[2], malformed[2];
};

#endif
