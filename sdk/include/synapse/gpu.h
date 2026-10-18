/*
    gpu.h: Запросы экрана и операций с изображением
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Запросы экрана и операций с изображением
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: 14.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYNAPSE_GPU_USER_H
#define SYNAPSE_GPU_USER_H
#include <stdint.h>
#define SYN_FB_GPU_INFO 0x4630
#define SYN_FB_CURSOR 0x4631
#define SYN_FB_BACKLIGHT_GET 0x4632
#define SYN_FB_BACKLIGHT_SET 0x4633
#define SYN_GPU_NATIVE_SCANOUT 1u
#define SYN_GPU_HW_CURSOR 2u
#define SYN_GPU_PWM_BACKLIGHT 4u
#define SYN_GPU_MIRROR 8u

struct syn_gpu_info {
	char name[32];
	uint32_t capabilities, pipe_mask, width, height, pitch;
	uint32_t reserved[3];
};

// Курсор имеет размер 64x64 и формат premultiplied ARGB
// Координаты учитывают активную точку курсора
struct syn_gpu_cursor {
	int32_t x, y;
	uint32_t visible, update_image;
	uint32_t pixels[64 * 64];
};

// Значения от 0 до 10000 задают яркость от 0 до 100 процентов
// Драйвер может ограничить минимум по данным VBT
struct syn_gpu_backlight {
	uint32_t level, minimum;
};

// Запросы /dev/fb0 описывают доступные операции с буфером изображения
// Операция на CPU сама по себе не означает поддержку GPU SDMA
#define SYN_FB_DISPLAY_QUERY_V1 0x4634
#define SYN_FB_2D_V1 0x4635
#define SYN_FB_MODE_SET_V1 0x4636
#define SYN_FB_FIXED_MODE 1u
#define SYN_FB_CPU_FILL 2u
#define SYN_FB_CPU_COPY 4u
#define SYN_FB_HW_FILL 8u
#define SYN_FB_HW_COPY 16u
#define SYN_FB_MODESET 32u
#define SYN_FB_FORMAT_XRGB8888 1u
#define SYN_FB_SOURCE_CPU 1u

struct syn_fb_display_v1 {
	uint32_t version, capabilities, mode_count, current_mode;
	uint32_t width, height, pitch, format;
	uint64_t buffer_bytes;
	uint32_t source, reserved[3];
};

#define SYN_FB_2D_FILL 1u
#define SYN_FB_2D_COPY 2u

// Операция синхронно меняет выделенный буфер /dev/fb0
// COPY допускает перекрытие как memmove
// Прямоугольники должны целиком помещаться в буфере
struct syn_fb_2d_v1 {
	uint32_t version, operation, sx, sy, dx, dy, width, height, color;
	uint32_t reserved[3];
};
#endif
