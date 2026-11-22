/*
    store_protocol.h: Протокол службы установки программ SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Ограниченный обмен между окном магазина и root-службой
    Подсистема: графический магазин программ
    Первое изменение: 01.11.2026
    Последнее изменение: 23.11.2026
    Доступ: общедоступный
*/

#ifndef SYNAPSE_STORE_PROTOCOL_H
#define SYNAPSE_STORE_PROTOCOL_H

#include <stdint.h>

#define STORE_SOCKET_PATH "/var/run/synapse-store.sock"
#define STORE_REQUEST_MAGIC UINT32_C(0x53534f53)
#define STORE_REQUEST_VERSION UINT32_C(5)
#define STORE_SOURCE_SETUP UINT32_C(1)
#define STORE_SOURCE_DEBIAN UINT32_C(2)
#define STORE_SOURCE_UPDATE UINT32_C(3)
#define STORE_SOURCE_DEBIAN_CATALOG UINT32_C(4)
#define STORE_SOURCE_SETUP_REMOVE UINT32_C(5)
#define STORE_SOURCE_DEBIAN_REMOVE UINT32_C(6)
#define STORE_SOURCE_UPGRADE UINT32_C(7)
#define STORE_SOURCE_SETTINGS UINT32_C(8)
#define STORE_SOURCE_POWER UINT32_C(9)
#define STORE_SOURCE_ACCOUNT_SETUP UINT32_C(10)
#define STORE_SOURCE_USERADD UINT32_C(11)
#define STORE_SOURCE_USERMOD UINT32_C(12)
#define STORE_SOURCE_PASSWD UINT32_C(13)
#define STORE_MAX_NAME 128
#define STORE_MAX_PASSWORD 256
#define STORE_MAX_OUTPUT 4096
#define STORE_MAX_DATA 256

struct store_request {
    uint32_t magic;
    uint32_t version;
    uint32_t source;
    uint32_t name_length;
    uint32_t password_length;
    uint32_t data_length;
    char name[STORE_MAX_NAME];
    char password[STORE_MAX_PASSWORD];
    char data[STORE_MAX_DATA];
};

#endif
