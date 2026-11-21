// Synapse Recorder, кодировщик H.264 для FFmpeg 7.1.3, 21.11.2026
#include "libavcodec/avcodec.h"
#include "libavcodec/codec_internal.h"
#include "libavcodec/encode.h"
#define H264E_MAX_THREADS 0
#define H264E_SVC_API 0
#include "minih264e.h"

struct encoder {
    H264E_persist_t *state;
    H264E_scratch_t *work;
    H264E_run_param_t run;
    int key;
    uint8_t headers[512];
    size_t length;
};
static void nal(const unsigned char *data, int size, void *token) {
    struct encoder *e=token;
    if (size<=0) return;
    int type=data[0]&31;
    if (type==5) e->key=1;
    if ((type==7 || type==8) && (size_t)size+4 <= sizeof(e->headers)-e->length) {
        memcpy(e->headers+e->length,"\0\0\0\1",4);
        memcpy(e->headers+e->length+4,data,size); e->length+=size+4;
    }
}
static int frame(AVCodecContext *c, AVPacket *pkt,const AVFrame *src,int *got) {
    struct encoder *e=c->priv_data;
    H264E_io_yuv_t in={0};
    for (int i=0;i<3;i++) { in.yuv[i]=src->data[i]; in.stride[i]=src->linesize[i]; }
    unsigned char *data; int size;
    e->key=0; e->length=0;
    if (H264E_encode(e->state,e->work,&e->run,&in,&data,&size) || size<=0) return AVERROR(EINVAL);
    if (pkt) {
        int r=ff_alloc_packet(c,pkt,size);
        if (r<0) return r;
        memcpy(pkt->data,data,size);
        pkt->pts=pkt->dts=src->pts;
        pkt->duration=1;
        if (e->key) pkt->flags|=AV_PKT_FLAG_KEY;
        *got=1;
    }
    return 0;
}
static int setup(AVCodecContext *c) {
    struct encoder *e=c->priv_data;
    H264E_create_param_t opts={.width=c->width,.height=c->height,.gop=c->gop_size,.const_input_flag=1};
    int state,work;
    if (H264E_sizeof(&opts,&state,&work) || state<=0 || work<=0) return AVERROR(EINVAL);
    e->state=av_mallocz(state); e->work=av_mallocz(work);
    if (!e->state || !e->work) return AVERROR(ENOMEM);
    if (H264E_init(e->state,&opts)) return AVERROR(EINVAL);
    e->run=(H264E_run_param_t){.encode_speed=H264E_SPEED_FASTEST,.desired_frame_bytes=c->bit_rate/8/c->framerate.num,
        .qp_min=10,.qp_max=48,.nalu_callback=nal,.nalu_callback_token=e};
    AVFrame *dummy=av_frame_alloc();
    if (!dummy) return AVERROR(ENOMEM);
    dummy->format=AV_PIX_FMT_YUV420P; dummy->width=c->width; dummy->height=c->height;
    int r=av_frame_get_buffer(dummy,32);
    if (!r) {
        for (int p=0;p<3;p++) memset(dummy->data[p],p ? 128 : 16,(size_t)dummy->linesize[p]*(p?c->height/2:c->height));
        r=frame(c,NULL,dummy,NULL);
    }
    av_frame_free(&dummy);
    if (r<0 || !e->length || H264E_init(e->state,&opts)) return AVERROR(EINVAL);
    c->extradata=av_mallocz(e->length+AV_INPUT_BUFFER_PADDING_SIZE);
    if (!c->extradata) return AVERROR(ENOMEM);
    memcpy(c->extradata,e->headers,e->length); c->extradata_size=e->length;
    return 0;
}
static int release(AVCodecContext *c) {
    struct encoder *e=c->priv_data; av_freep(&e->state); av_freep(&e->work); return 0;
}
static const FFCodec encoder={
    .p.name="studio-h264", CODEC_LONG_NAME("minih264 H.264"),
    .p.type=AVMEDIA_TYPE_VIDEO,.p.id=AV_CODEC_ID_H264,.priv_data_size=sizeof(struct encoder),
    .init=setup,.close=release,.caps_internal=FF_CODEC_CAP_INIT_CLEANUP,
    FF_CODEC_ENCODE_CB(frame),
    .p.pix_fmts=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NONE}
};
const AVCodec *studio_h264(void) { return &encoder.p; }
