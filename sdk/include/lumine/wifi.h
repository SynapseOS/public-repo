/*
    wifi.h: Интерфейс управления беспроводным устройством
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: SynapseOS userspace/kernel

    Описание: Команды /dev/wifi и фиксированные структуры обмена
    Подсистема: беспроводная сеть
    Первое изменение: 16.11.2026
    Последнее изменение: 16.11.2026
    Доступ: общедоступный
*/

#ifndef SYNAPSE_WIFI_H
#define SYNAPSE_WIFI_H

#include <stdint.h>

#define SYN_WIFI_ABI_VERSION 1U
#define SYN_WIFI_SCAN_LIMIT 32U
#define SYN_WIFI_SSID_LIMIT 32U
#define SYN_WIFI_PASSWORD_LIMIT 63U

#define SYN_WIFI_IOCTL_SCAN 0x57574601U
#define SYN_WIFI_IOCTL_CONNECT 0x57574602U
#define SYN_WIFI_IOCTL_STATUS 0x57574603U
#define SYN_WIFI_IOCTL_DISCONNECT 0x57574604U
#define SYN_WIFI_IOCTL_SET_ENABLED 0x57574605U

#define SYN_WIFI_SECURITY_OPEN 0U
#define SYN_WIFI_SECURITY_WPA2_PSK 1U

struct syn_wifi_bss {
    uint8_t ssid[SYN_WIFI_SSID_LIMIT];
    uint8_t ssid_length;
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi_dbm;
    uint8_t security;
};

struct syn_wifi_scan {
    uint32_t version;
    uint32_t count;
    struct syn_wifi_bss entries[SYN_WIFI_SCAN_LIMIT];
};

struct syn_wifi_connect {
    uint32_t version;
    uint8_t ssid[SYN_WIFI_SSID_LIMIT];
    uint8_t ssid_length;
    uint8_t password[SYN_WIFI_PASSWORD_LIMIT];
    uint8_t password_length;
};

struct syn_wifi_status {
    uint32_t version;
    uint8_t enabled;
    uint8_t associated;
    uint8_t ready;
    uint8_t reserved;
    uint8_t ssid[SYN_WIFI_SSID_LIMIT];
    uint8_t ssid_length;
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi_dbm;
    uint32_t ipv4;
    uint32_t gateway;
    uint32_t link_mbps;
};

struct syn_wifi_enabled {
    uint32_t version;
    uint32_t enabled;
};

#endif
