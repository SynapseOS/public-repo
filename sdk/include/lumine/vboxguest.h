/*
    vboxguest.h: Интерфейс службы VirtualBox
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Интерфейс службы VirtualBox
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: не установлено по истории Git
    Последнее изменение: 18.10.2026
*/

#ifndef LUMINE_VBOXGUEST_H
#define LUMINE_VBOXGUEST_H
#include <stdint.h>
// Интерфейс SynapseOS для службы VirtualBox
// Запросы Linux VBoxGuest через него не передаются
#define SYN_VBOX_ABI_VERSION 1
#define SYN_VBOX_INFO 0x56420001U
#define SYN_VBOX_START 0x56420002U
#define SYN_VBOX_PULSE 0x56420003U
#define SYN_VBOX_STOP 0x56420004U

struct syn_vbox_info {
	uint32_t abi_version, host_major, host_minor, host_build;
	uint32_t host_revision, host_features, service_active, heartbeat_enabled;
	uint64_t host_time_ms, heartbeat_interval_ns, requests;
	int32_t last_rc;
	uint32_t reserved;
};

_Static_assert(sizeof(struct syn_vbox_info) == 64, "VBoxGuest ABI");
#endif
