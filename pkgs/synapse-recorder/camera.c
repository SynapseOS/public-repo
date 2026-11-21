// Synapse Recorder, захват UVC MJPEG и YUY2, 21.11.2026
#include "studio.h"
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <lumine/uvc.h>

int camera_open(struct camera *c, const char *path, int wanted) {
    c->fd = open(path, O_RDWR | O_CLOEXEC);
    if (c->fd < 0) return -1;
    struct syn_uvc_mode m;
    for (unsigned i = 0; ; i++) {
        memset(&m, 0, sizeof(m));
        m.index = i;
        if (ioctl(c->fd, SYN_UVC_ENUM, &m) < 0) return -1;
        if (wanted >= 0 && i != (unsigned)wanted) continue;
        if (m.subtype == 6 || !memcmp(m.guid, "YUY2", 4)) break;
        if (wanted >= 0) return -1;
    }
    c->mjpeg = m.subtype == 6;
    c->w = m.width;
    c->h = m.height;
    if (!c->w || !c->h || c->w > 8192 || c->h > 8192 || (!c->mjpeg && c->w % 2)) return -1;
    struct syn_uvc_start start = {.index = m.index, .interval = m.minimum};
    if (ioctl(c->fd, SYN_UVC_START, &start) < 0 || !start.frame_bytes || start.frame_bytes > 128u*1024*1024)
        return -1;
    c->capacity = start.frame_bytes;
    c->buffer = av_mallocz(c->capacity + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!c->buffer) return -1;
    if (c->mjpeg) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        c->decoder = avcodec_alloc_context3(codec);
        c->frame = av_frame_alloc();
        if (!codec || !c->decoder || !c->frame || avcodec_open2(c->decoder, codec, NULL) < 0) return -1;
    }
    return 0;
}

int camera_read(struct camera *c, struct image *out) {
    struct syn_uvc_frame f = {.address = (uintptr_t)c->buffer, .capacity = c->capacity};
    if (ioctl(c->fd, SYN_UVC_FRAME, &f) < 0 || !f.used || f.used > f.capacity) return -1;
    const uint8_t *data[4] = {c->buffer};
    int lines[4] = {c->w * 2};
    enum AVPixelFormat format = AV_PIX_FMT_YUYV422;
    if (c->mjpeg) {
        memset(c->buffer + f.used, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        AVPacket pkt = {.data = c->buffer, .size = (int)f.used};
        if (avcodec_send_packet(c->decoder, &pkt) < 0 || avcodec_receive_frame(c->decoder, c->frame) < 0 ||
            c->frame->width != c->w || c->frame->height != c->h) return -1;
        format = c->frame->format;
        memcpy(data, c->frame->data, sizeof(data));
        memcpy(lines, c->frame->linesize, sizeof(lines));
    } else if (f.used != (unsigned)c->h * c->w * 2) return -1;
    c->scale = sws_getCachedContext(c->scale, c->w, c->h, format, out->w, out->h,
                                    AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t *dest[4] = {out->pixels};
    int strides[4] = {out->stride};
    return c->scale && sws_scale(c->scale, data, lines, 0, c->h, dest, strides) == out->h ? 0 : -1;
}

void camera_close(struct camera *c) {
    if (c->fd >= 0) { if (c->capacity) ioctl(c->fd, SYN_UVC_STOP, 0); close(c->fd); }
    avcodec_free_context(&c->decoder);
    av_frame_free(&c->frame);
    av_free(c->buffer);
    sws_freeContext(c->scale);
}
