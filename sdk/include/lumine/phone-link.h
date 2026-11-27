// phone-link.h: Передача приложения в RAM телефона по USB
// Автор: Елчинян Арен Артурович, 2026, ABI little-endian ARM64 и x86_64
// Подсистема USB, первое и последнее изменение 27.11.2026, доступ общедоступный
#ifndef SYN_PHONE_LINK_H
#define SYN_PHONE_LINK_H
#include <stdint.h>
#define SYN_LINK_LIMIT (2u * 1024u * 1024u)
#define SYN_LINK_SEND 0x53595002u
#define SYN_LINK_QUERY 0x53595003u
#define SYN_LINK_FETCH 0x53595004u
#define SYN_LINK_FINISH 0x53595005u
enum syn_link_op { SYN_LINK_INSTALL = 1, SYN_LINK_REMOVE, SYN_LINK_DATA };
enum syn_link_state { SYN_LINK_IDLE, SYN_LINK_RECEIVING, SYN_LINK_READY, SYN_LINK_DONE, SYN_LINK_ERROR };
struct syn_link_frame {
    uint32_t token, op, offset, length;
    uint8_t data[496];
};
struct syn_link_status {
    uint32_t token, state, received, total;
};
#endif
