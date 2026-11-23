/*
    bthci.h: Интерфейс транспорта HCI через /dev/bthciN
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: SynapseOS userspace/kernel

    Описание: Команды ioctl устройств /dev/bthciN и структуры обмена
    Подсистема: Bluetooth
    Первое изменение: 23.11.2026
    Последнее изменение: 23.11.2026
    Доступ: коммерческий
*/

#ifndef SYNAPSE_BTHCI_H
#define SYNAPSE_BTHCI_H

#include <stdint.h>

#define SYN_BTHCI_ABI_VERSION 1U

// Устройство принимает и отдаёт поток кадров H4
// write: 0x01 HCI command, 0x02 ACL data, 0x03 SCO data
// read: 0x04 HCI event, 0x02 ACL data, 0x03 SCO data
#define SYN_BTHCI_H4_COMMAND 0x01U
#define SYN_BTHCI_H4_ACL 0x02U
#define SYN_BTHCI_H4_SCO 0x03U
#define SYN_BTHCI_H4_EVENT 0x04U

#define SYN_BTHCI_IOCTL_INFO 0x42544801U
#define SYN_BTHCI_IOCTL_FLUSH 0x42544802U
#define SYN_BTHCI_IOCTL_ISO_OPEN 0x42544803U
#define SYN_BTHCI_IOCTL_ISO_CLOSE 0x42544804U
#define SYN_BTHCI_IOCTL_SET_ALT 0x42544805U
#define SYN_BTHCI_IOCTL_CLEAR_HALT 0x42544806U

// Флаги поля syn_bthci_info.flags
#define SYN_BTHCI_INFO_ISO_OPEN 0x01U

struct syn_bthci_info {
    uint32_t version;
    uint16_t vendor;
    uint16_t product;
    uint8_t speed;
    uint8_t interface;
    uint8_t index;
    uint8_t flags;
    uint8_t event_endpoint;
    uint8_t acl_in_endpoint;
    uint8_t acl_out_endpoint;
    uint8_t reserved[5];
};

// Параметры изохронной конечной точки SCO
// Значения берутся из выбранной альтернативы интерфейса
struct syn_bthci_iso {
    uint32_t version;
    uint8_t address;
    uint8_t interval;
    uint8_t burst;
    uint8_t mult;
    uint16_t packet;
    uint16_t bytes;
};

struct syn_bthci_alt {
    uint32_t version;
    uint8_t interface;
    uint8_t alternate;
    uint8_t reserved[2];
};

#endif
