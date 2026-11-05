/*
    store_protocol.h: Протокол службы установки программ SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Ограниченный обмен между окном магазина и root-службой
    Подсистема: графический магазин программ
    Первое изменение: 01.11.2026
    Последнее изменение: 03.11.2026
    Доступ: общедоступный
*/

#ifndef SYNAPSE_STORE_PROTOCOL_H
#define SYNAPSE_STORE_PROTOCOL_H

#include <stdint.h>

#define STORE_SOCKET_PATH "/var/run/synapse-store.sock"
#define STORE_REQUEST_MAGIC UINT32_C(0x53534f53)
#define STORE_REQUEST_VERSION UINT32_C(3)
#define STORE_SOURCE_SETUP UINT32_C(1)
#define STORE_SOURCE_DEBIAN UINT32_C(2)
#define STORE_SOURCE_UPDATE UINT32_C(3)
#define STORE_SOURCE_DEBIAN_CATALOG UINT32_C(4)
#define STORE_MAX_NAME 128
#define STORE_MAX_PASSWORD 256
#define STORE_MAX_OUTPUT 4096

struct store_request {
    uint32_t magic;
    uint32_t version;
    uint32_t source;
    uint32_t name_length;
    uint32_t password_length;
    char name[STORE_MAX_NAME];
    char password[STORE_MAX_PASSWORD];
};

#endif
