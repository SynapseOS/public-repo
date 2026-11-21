// Synapse Recorder, снимки JPEG/PNG и логотипы с прозрачностью, 21.11.2026
#include "studio.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

static enum AVCodecID kind(const char *path) {
    const char *ext=strrchr(path,'.');
    if (!ext) return AV_CODEC_ID_NONE;
    if (!strcasecmp(ext,".png")) return AV_CODEC_ID_PNG;
    if (!strcasecmp(ext,".jpg") || !strcasecmp(ext,".jpeg")) return AV_CODEC_ID_MJPEG;
    if (!strcasecmp(ext,".bmp")) return AV_CODEC_ID_BMP;
    return AV_CODEC_ID_NONE;
}
int bitmap_write(const char *,const struct image *);
int snapshot(const char *path,const struct image *im) {
    enum AVCodecID id=kind(path);
    if (id==AV_CODEC_ID_BMP) return bitmap_write(path,im);
    const AVCodec *codec=avcodec_find_encoder(id);
    AVCodecContext *c=codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *f=av_frame_alloc(); AVPacket *p=av_packet_alloc();
    struct SwsContext *scale=NULL;
    int fd=-1, ok=0;
    if (!c || !f || !p) goto done;
    c->width=im->w; c->height=im->h; c->time_base=(AVRational){1,1};
    c->pix_fmt=id==AV_CODEC_ID_PNG ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_YUVJ444P;
    c->thread_count=1;
    if (avcodec_open2(c,codec,NULL)<0) goto done;
    f->width=c->width; f->height=c->height; f->format=c->pix_fmt;
    if (av_frame_get_buffer(f,32)<0) goto done;
    scale=sws_getContext(im->w,im->h,AV_PIX_FMT_BGR0,im->w,im->h,c->pix_fmt,SWS_BILINEAR,NULL,NULL,NULL);
    const uint8_t *data[]={im->pixels};
    if (!scale || sws_scale(scale,data,&im->stride,0,im->h,f->data,f->linesize)!=im->h ||
        avcodec_send_frame(c,f)<0 || avcodec_receive_packet(c,p)<0) goto done;
    fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
    if (fd<0) { perror(path); goto done; }
    for (int off=0;off<p->size;) {
        ssize_t n=write(fd,p->data+off,p->size-off);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) goto done;
        off+=n;
    }
    ok=1;
done:
    if (fd>=0) { if (close(fd)) ok=0; if (!ok) unlink(path); }
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&c); sws_freeContext(scale);
    return ok ? 0 : -1;
}
int load_logo(const char *path,struct image *im) {
    const AVCodec *codec=avcodec_find_decoder(kind(path));
    FILE *in=fopen(path,"rb");
    AVCodecContext *c=codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *f=av_frame_alloc(); AVPacket *p=av_packet_alloc();
    struct SwsContext *scale=NULL;
    int ok=0;
    if (!in || !c || !p || !f || fseek(in,0,SEEK_END)) goto done;
    long len=ftell(in);
    if (len<=0 || len>16*1024*1024 || fseek(in,0,SEEK_SET) || av_new_packet(p,len)<0 ||
        fread(p->data,1,len,in)!=(size_t)len) goto done;
    c->max_pixels=16777216; c->thread_count=1;
    if (avcodec_open2(c,codec,NULL)<0 || avcodec_send_packet(c,p)<0 || avcodec_receive_frame(c,f)<0 ||
        allocate_image(im,f->width,f->height)<0) goto done;
    scale=sws_getContext(f->width,f->height,f->format,im->w,im->h,AV_PIX_FMT_BGRA,SWS_BILINEAR,NULL,NULL,NULL);
    uint8_t *dst[]={im->pixels};
    if (scale && sws_scale(scale,(const uint8_t *const *)f->data,f->linesize,0,f->height,dst,&im->stride)==im->h) ok=1;
done:
    if (in) fclose(in);
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&c); sws_freeContext(scale);
    return ok ? 0 : -1;
}
