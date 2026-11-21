// uvc.h: Интерфейс захвата видео USB, автор Елчинян Арен Артурович, 2026
// x86_64, ABI UVC 1, подсистема видео, 21.11.2026, коммерческий
#ifndef LUMINE_UVC_H
#define LUMINE_UVC_H
#include <stdint.h>

// Интервалы заданы в единицах 100 нс согласно UVC
#define SYN_UVC_ENUM  0x55560101u
#define SYN_UVC_START 0x55560102u
#define SYN_UVC_FRAME 0x55560103u
#define SYN_UVC_STOP  0x55560104u

struct syn_uvc_mode {
    uint32_t index, minimum, maximum, step, max_bytes;
    uint16_t width, height;
    uint8_t format, frame, subtype, bits, guid[16];
};

struct syn_uvc_start {
    uint32_t index, interval, frame_bytes;
};

struct syn_uvc_frame {
    uint64_t address;
    uint32_t capacity, used;
};
#endif
