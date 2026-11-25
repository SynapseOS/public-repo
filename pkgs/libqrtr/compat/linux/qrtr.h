// qrtr.h: Совместимые объявления стандартного Linux AF_QIPCRTR ABI
// Автор: Елчинян Арен Артурович 2026
// Архитектура: AArch64 и x86_64, ABI: sockaddr_qrtr и qrtr_ctrl_pkt
// Описание: Только публичные структуры и номера команд, реализации ядра здесь нет
// Подсистема: публичный SDK
// Первое изменение: 25.11.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный
#ifndef SYN_COMPAT_LINUX_QRTR_H
#define SYN_COMPAT_LINUX_QRTR_H
#include <stdint.h>
typedef uint32_t __le32;
#define QRTR_NODE_BCAST UINT32_C(0xffffffff)
#define QRTR_PORT_CTRL UINT32_C(0xfffffffe)
struct sockaddr_qrtr { uint16_t sq_family; uint32_t sq_node, sq_port; };
enum qrtr_pkt_type {
    QRTR_TYPE_DATA = 1, QRTR_TYPE_HELLO, QRTR_TYPE_BYE, QRTR_TYPE_NEW_SERVER,
    QRTR_TYPE_DEL_SERVER, QRTR_TYPE_DEL_CLIENT, QRTR_TYPE_RESUME_TX,
    QRTR_TYPE_EXIT, QRTR_TYPE_PING, QRTR_TYPE_NEW_LOOKUP, QRTR_TYPE_DEL_LOOKUP,
};
struct qrtr_ctrl_pkt {
    __le32 cmd;
    union {
        struct { __le32 service, instance, node, port; } server;
        struct { __le32 node, port; } client;
    };
} __attribute__((packed));
#endif
