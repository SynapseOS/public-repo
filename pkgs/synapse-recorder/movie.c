// Synapse Recorder, кодирование видео и звука, 21.11.2026
#include "studio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavutil/opt.h>

static int output(void *opaque, const uint8_t *p, int size) {
    int fd = *(int *)opaque, pos = 0;
    while (pos < size) {
        ssize_t n = write(fd, p + pos, size - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return AVERROR(n < 0 ? errno : EIO);
        pos += n;
    }
    return pos;
}

static int64_t seek_output(void *opaque, int64_t off, int whence) {
    int fd = *(int *)opaque;
    if (whence == AVSEEK_SIZE) {
        struct stat st;
        return fstat(fd, &st) ? AVERROR(errno) : st.st_size;
    }
    off_t result = lseek(fd, off, whence & ~AVSEEK_FORCE);
    return result < 0 ? AVERROR(errno) : result;
}

static int stream_open(struct movie *m, AVCodecContext *c, AVStream **stream) {
    *stream = avformat_new_stream(m->fmt, NULL);
    if (!*stream) return -1;
    if (m->fmt->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, c->codec, NULL) < 0) return -1;
    (*stream)->time_base = c->time_base;
    return avcodec_parameters_from_context((*stream)->codecpar, c);
}

const AVCodec *studio_h264(void);

int movie_open(struct movie *m, const char *path, int w, int h, int fps, int mbps, int audio, int h264) {
    const char *ext = strrchr(path, '.');
    const char *format = !ext ? NULL : !strcmp(ext, ".mkv") ? "matroska" :
        !strcmp(ext, ".avi") ? "avi" : !strcmp(ext, ".mp4") ? "mp4" : NULL;
    if (!format || avformat_alloc_output_context2(&m->fmt, NULL, format, path) < 0) return -1;
    w=(w+1)&~1; h=(h+1)&~1;
    const AVCodec *v = h264 ? studio_h264() : avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!v || !(m->video = avcodec_alloc_context3(v))) return -1;
    AVCodecContext *c = m->video;
    c->width = w;
    c->height = h;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = (AVRational){1, fps};
    c->framerate = (AVRational){fps, 1};
    c->bit_rate = (int64_t)mbps * 1000000;
    c->gop_size = fps * 2;
    c->thread_count = 1;
    if (stream_open(m, c, &m->vs) < 0) return -1;
    m->vs->avg_frame_rate = c->framerate;
    m->vf = av_frame_alloc();
    if (!m->vf) return -1;
    m->vf->width = w;
    m->vf->height = h;
    m->vf->format = c->pix_fmt;
    if (av_frame_get_buffer(m->vf, 32) < 0) return -1;
    if (audio) {
        const AVCodec *a = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!a || !(m->audio = avcodec_alloc_context3(a))) return -1;
        c = m->audio;
        c->sample_rate = 48000;
        c->sample_fmt = AV_SAMPLE_FMT_FLTP;
        c->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        c->time_base = (AVRational){1, 48000};
        c->bit_rate = 128000;
        if (stream_open(m, c, &m->as) < 0) return -1;
        m->af = av_frame_alloc();
        if (!m->af || c->frame_size != 1024) return -1;
        m->af->format = c->sample_fmt;
        m->af->nb_samples = c->frame_size;
        m->af->sample_rate = c->sample_rate;
        if (av_channel_layout_copy(&m->af->ch_layout, &c->ch_layout) < 0 || av_frame_get_buffer(m->af, 0) < 0)
            return -1;
    }
    m->packet = av_packet_alloc();
    if (!m->packet) return -1;
    m->fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (m->fd < 0) { perror(path); return -1; }
    m->owned = 1;
    uint8_t *buf = av_malloc(32768);
    if (!buf) return -1;
    m->fmt->pb = avio_alloc_context(buf, 32768, 1, &m->fd, NULL, output, seek_output);
    if (!m->fmt->pb) { av_free(buf); return -1; }
    m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_write_header(m->fmt, NULL) < 0) return -1;
    m->header = 1;
    return 0;
}

static int encode(struct movie *m, AVCodecContext *c, AVStream *s, AVFrame *frame) {
    int r = avcodec_send_frame(c, frame);
    if (r < 0) return r;
    while ((r = avcodec_receive_packet(c, m->packet)) >= 0) {
        av_packet_rescale_ts(m->packet, c->time_base, s->time_base);
        m->packet->stream_index = s->index;
        if ((r = av_interleaved_write_frame(m->fmt, m->packet)) < 0) return r;
    }
    return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
}

int movie_video(struct movie *m, const struct image *im, int64_t pts) {
    if (av_frame_make_writable(m->vf) < 0) return -1;
    // BT.601 limited range, один отсчёт цветности на квадрат 2 x 2
    for (int y=0; y<m->vf->height; y+=2) {
        for (int x=0; x<m->vf->width; x+=2) {
            int red=0, green=0, blue=0;
            for (int dy=0; dy<2; dy++) {
                uint8_t *luma=m->vf->data[0]+(y+dy)*m->vf->linesize[0];
                const uint8_t *row=im->pixels+FFMIN(y+dy,im->h-1)*im->stride;
                for (int dx=0; dx<2; dx++) {
                    const uint8_t *pixel=row+4*FFMIN(x+dx,im->w-1);
                    int b=pixel[0], g=pixel[1], r=pixel[2];
                    luma[x+dx]=16+((66*r+129*g+25*b+128)>>8);
                    red+=r; green+=g; blue+=b;
                }
            }
            m->vf->data[1][y/2*m->vf->linesize[1]+x/2]=(131584-38*red-74*green+112*blue)>>10;
            m->vf->data[2][y/2*m->vf->linesize[2]+x/2]=(131584+112*red-94*green-18*blue)>>10;
        }
    }
    m->vf->pts = pts;
    int r = encode(m, m->video, m->vs, m->vf);
    if (!r) m->frames++;
    return r;
}

int movie_audio(struct movie *m, const int16_t *pcm) {
    if (av_frame_make_writable(m->af) < 0) return -1;
    for (int ch = 0; ch < 2; ch++) {
        float *dst = (float *)m->af->data[ch];
        for (int i = 0; i < 1024; i++) dst[i] = pcm[2*i + ch] / 32768.f;
    }
    m->af->pts = m->samples;
    m->samples += 1024;
    return encode(m, m->audio, m->as, m->af);
}

int movie_close(struct movie *m) {
    int r = 0;
    if (m->header) {
        if (encode(m, m->video, m->vs, NULL) < 0) r = -1;
        if (m->audio && encode(m, m->audio, m->as, NULL) < 0) r = -1;
        if (av_write_trailer(m->fmt) < 0) r = -1;
    }
    if (m->fmt && m->fmt->pb) {
        avio_flush(m->fmt->pb);
        if (m->fmt->pb->error) r = -1;
        av_freep(&m->fmt->pb->buffer);
        avio_context_free(&m->fmt->pb);
    }
    if (m->fd >= 0 && close(m->fd)) r = -1;
    av_frame_free(&m->af);
    av_frame_free(&m->vf);
    av_packet_free(&m->packet);
    avcodec_free_context(&m->audio);
    avcodec_free_context(&m->video);
    avformat_free_context(m->fmt);
    sws_freeContext(m->scale);
    return r;
}
