// Synapse Recorder, мультимедиа SynapseOS, 21.11.2026
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

struct image { int w, h, stride; uint8_t *pixels; };
struct movie {
    int fd, header, owned, frames;
    AVFormatContext *fmt;
    AVCodecContext *video, *audio;
    AVStream *vs, *as;
    AVFrame *vf, *af;
    AVPacket *packet;
    struct SwsContext *scale;
    int64_t samples;
};
int movie_open(struct movie *, const char *, int, int, int, int, int, int);
int movie_video(struct movie *, const struct image *, int64_t);
int movie_audio(struct movie *, const int16_t *);
int movie_close(struct movie *);
int snapshot(const char *, const struct image *);
int allocate_image(struct image *, int, int);
int load_logo(const char *, struct image *);
void composite(struct image *, const struct image *, int, int, int, int);
void caption(struct image *, const char *, int, int);
struct camera {
    int fd, mjpeg, w, h;
    uint8_t *buffer;
    unsigned capacity;
    AVCodecContext *decoder;
    AVFrame *frame;
    struct SwsContext *scale;
};
int camera_open(struct camera *, const char *, int);
int camera_read(struct camera *, struct image *);
void camera_close(struct camera *);
