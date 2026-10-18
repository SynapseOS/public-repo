#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "device.h"
#include "genfont.h"
#include "genmem.h"
#include "fb.h"
#include "lumine_fb.h"
static int framebuffer = -1;

static void update(PSD p, int x, int y, int w, int h) {
	if (w <= 0 || h <= 0) {
		x = 0;
		y = 0;
		w = p->xres;
		h = p->yres;
	}

	struct {
		unsigned x, y, w, h;
	} rect = { x, y, w, h };

	/* Aggregate dirty primitives; flush once at the event-loop boundary. */
	ioctl(framebuffer, 0x4621, &rect);
}

static int preselect(PSD p) {
	(void)p;
	ioctl(framebuffer, 0x4622, 0);
	return 1;
}

static PSD open_screen(PSD p) {
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	framebuffer = open("/dev/fb0", O_RDWR);

	if (framebuffer < 0 || ioctl(framebuffer, 0x4600, &var) || ioctl(framebuffer, 0x4602, &fix))
		goto fail;
	if (var.bits_per_pixel != 32 || var.red.offset != 16 || var.green.offset != 8 ||
	    var.blue.offset != 0)
		goto fail;
	p->xres = p->xvirtres = var.xres;
	p->yres = p->yvirtres = var.yres;
	p->bpp = 32;
	p->pitch = fix.line_length;
	p->size = p->pitch * p->yres;
	p->planes = 1;
	p->ncolors = 1 << 24;
	p->flags = PSF_SCREEN | PSF_DELAYUPDATE;
	p->portrait = MWPORTRAIT_NONE;
	p->pixtype = MWPF_TRUECOLORARGB;
	p->data_format = set_data_format(p);
	PSUBDRIVER sub = select_fb_subdriver(p);

	if (!sub)
		goto fail;
	set_subdriver(p, sub);
	p->addr = mmap(NULL, p->size, PROT_READ | PROT_WRITE, MAP_SHARED, framebuffer, 0);

	if (p->addr == MAP_FAILED)
		goto fail;
	if (ioctl(framebuffer, 0x4626, 0)) {
		munmap(p->addr, p->size);
		goto fail;
	}

	return p;
fail:
	perror("nano-x framebuffer");

	if (framebuffer >= 0)
		close(framebuffer);
	framebuffer = -1;
	return NULL;
}

static void close_screen(PSD p) {
	if (framebuffer >= 0) {
		ioctl(framebuffer, 0x4627, 0);
		munmap(p->addr, p->size);
		close(framebuffer);
		framebuffer = -1;
	}
}

static void palette(PSD p, int first, int count, MWPALENTRY *entries) {
	(void)p;
	(void)first;
	(void)count;
	(void)entries;
}

SCREENDEVICE scrdev = { .builtin_fonts = gen_fonts,
	                    .Open = open_screen,
	                    .Close = close_screen,
	                    .SetPalette = palette,
	                    .GetScreenInfo = gen_getscreeninfo,
	                    .AllocateMemGC = gen_allocatememgc,
	                    .MapMemGC = gen_mapmemgc,
	                    .FreeMemGC = gen_freememgc,
	                    .SetPortrait = gen_setportrait,
	                    .Update = update,
	                    .PreSelect = preselect };
