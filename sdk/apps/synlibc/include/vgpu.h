#ifndef SYNLIBC_VGPU_H
#define SYNLIBC_VGPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct syn_vgpu_info {
	uint32_t width;
	uint32_t height;
	uint32_t bpp; // Bits per pixel
	uint32_t pitch; // Bytes per line (width * (bpp/8))
};

int syn_vgpu_get_info(struct syn_vgpu_info *info);
int syn_vgpu_alloc_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int syn_vgpu_free_window(int id);
int syn_vgpu_blit(int id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *buf);
int syn_vgpu_draw_rect(int id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
int syn_vgpu_flush(int id);

#endif