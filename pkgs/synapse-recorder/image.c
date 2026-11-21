// Synapse Recorder, снимки BMP и композиция, 21.11.2026
#include "studio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const uint8_t avpriv_cga_font[2048];

int allocate_image(struct image *im, int w, int h) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (uint64_t)w * h > 33554432) return -1;
    im->w = w;
    im->h = h;
    im->stride = w * 4;
    im->pixels = av_mallocz((size_t)im->stride * h + AV_INPUT_BUFFER_PADDING_SIZE);
    return im->pixels ? 0 : -1;
}

static void put32(uint8_t *p, uint32_t n) {
    for (int i = 0; i < 4; i++) p[i] = n >> (i * 8);
}

int bitmap_write(const char *path, const struct image *im) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { perror(path); return -1; }
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(path); return -1; }
    uint8_t hdr[54] = {'B','M'};
    put32(hdr+2, 54u + im->w * im->h * 4u);
    put32(hdr+10, 54);
    put32(hdr+14, 40);
    put32(hdr+18, im->w);
    put32(hdr+22, -(uint32_t)im->h);
    hdr[26] = 1;
    hdr[28] = 32;
    int ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (int y = 0; ok && y < im->h; y++) ok = fwrite(im->pixels+y*im->stride, 4, im->w, f) == (size_t)im->w;
    if (fclose(f) || !ok) { unlink(path); return -1; }
    return 0;
}

void composite(struct image *dst, const struct image *src, int right, int bottom, int width, int blend) {
    if (!src->pixels) return;
    int w = FFMIN(width, dst->w), h = FFMAX(1, (int)((int64_t)w * src->h / src->w));
    if (h > dst->h) { h = dst->h; w = FFMAX(1, (int)((int64_t)h * src->w / src->h)); }
    int margin = FFMIN(16, FFMIN(dst->w-w, dst->h-h) / 2);
    int left = right ? dst->w - margin - w : margin, top = bottom ? dst->h - margin - h : margin;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src->pixels + (int64_t)y * src->h / h * src->stride;
        uint8_t *d = dst->pixels + (top + y) * dst->stride + left * 4;
        for (int x = 0; x < w; x++) {
            const uint8_t *p=s+((int64_t)x*src->w/w)*4;
            uint8_t *q=d+x*4;
            if (!blend || p[3]==255) memcpy(q,p,4);
            else for (int k=0;k<3;k++) q[k]=(p[k]*p[3]+q[k]*(255-p[3])+127)/255;
        }
    }
}

void caption(struct image *im, const char *s, int x, int y) {
    for (; *s && x + 8 <= im->w && y + 12 <= im->h; s++, x += 8) {
        const uint8_t *glyph = avpriv_cga_font + (uint8_t)*s * 8;
        for (int row = 0; row < 12; row++) for (int col = 0; col < 8; col++) {
            uint8_t *p = im->pixels + (row+y)*im->stride + (col+x)*4;
            int lit = row >= 2 && row < 10 && (glyph[row-2] & (128 >> col));
            for (int c = 0; c < 3; c++) p[c] = lit ? 253 : p[c] / 3;
        }
    }
}
