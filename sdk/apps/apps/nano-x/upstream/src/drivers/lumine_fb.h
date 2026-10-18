#ifndef DOOM_FRAMEBUFFER_H
#define DOOM_FRAMEBUFFER_H
#include <stdint.h>

struct fb_fix_screeninfo {
	char id[16];
	unsigned long smem_start;
	uint32_t smem_len;
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint32_t xpanstep;
	uint32_t ypanstep;
	uint32_t ywrapstep;
	uint32_t line_length;
	unsigned long mmio_start;
	uint32_t mmio_len;
	uint32_t accel;
	uint16_t capabilities;
	uint16_t reserved[2];
} __attribute__((packed));

struct fb_bitfield {
	uint32_t offset;
	uint32_t length;
	uint32_t msb_right;
} __attribute__((packed));

struct fb_var_screeninfo {
	uint32_t xres;
	uint32_t yres;
	uint32_t xres_virtual;
	uint32_t yres_virtual;
	uint32_t xoffset;
	uint32_t yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;
	uint32_t nonstd;
	uint32_t activate;
	uint32_t height;
	uint32_t width;
	uint32_t accel_flags;
	uint32_t pixclock;
	uint32_t left_margin;
	uint32_t right_margin;
	uint32_t upper_margin;
	uint32_t lower_margin;
	uint32_t hsync_len;
	uint32_t vsync_len;
	uint32_t sync;
	uint32_t vmode;
	uint32_t rotate;
	uint32_t colorspace;
	uint32_t reserved[4];
} __attribute__((packed));

#endif
