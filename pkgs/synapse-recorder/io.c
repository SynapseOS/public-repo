// Synapse Recorder, экран SynapseOS и источники PCM, 21.11.2026
#define _GNU_SOURCE
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <synapse/gpu.h>

int64_t monotime(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void reap(pid_t pid) {
    if (pid > 0) {
        kill(pid,SIGKILL);
        while (waitpid(pid,NULL,0) < 0 && errno == EINTR) {}
    }
}

static int receive(int fd, void *dst, size_t len) {
    int64_t end = monotime() + 5000000;
    size_t pos = 0;
    while (pos < len) {
        int64_t ms = (end - monotime()) / 1000;
        if (ms <= 0) return -1;
        struct pollfd p = {fd,POLLIN,0};
        int r = poll(&p,1,(int)ms);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        ssize_t n = read(fd, (uint8_t *)dst + pos, len-pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        pos += n;
    }
    return 0;
}

int screen_open(struct screen *s, int wayland, const char *monitor, const char *window, int cursor) {
    s->wayland = wayland;
    if (wayland) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
        s->child = fork();
        if (!s->child) {
            close(pair[0]);
            if (dup2(pair[1],0) < 0 || dup2(pair[1],1) < 0) _exit(126);
            if (pair[1] > 1) close(pair[1]);
            char *args[7]={"recorder-grab"}; int n=1;
            if (monitor) { args[n++]="--monitor"; args[n++]=(char *)monitor; }
            if (window) { args[n++]="--window"; args[n++]=(char *)window; }
            if (!cursor) args[n++]="--hide-cursor";
            execv("/usr/libexec/recorder-grab",args);
            _exit(127);
        }
        close(pair[1]);
        s->fd = pair[0];
        fcntl(s->fd,F_SETFD,FD_CLOEXEC);
        uint32_t geo[2];
        if (s->child < 0 || write(s->fd,"F",1) != 1 || receive(s->fd,geo,sizeof(geo)) < 0 ||
            allocate_image(&s->im,geo[0],geo[1]) < 0) return -1;
        s->first = 1;
        return 0;
    }
    s->fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    struct syn_fb_display_v1 fb = {.version = 1};
    if (s->fd < 0 || ioctl(s->fd,SYN_FB_DISPLAY_QUERY_V1,&fb) < 0 || fb.format != SYN_FB_FORMAT_XRGB8888 ||
        !fb.width || !fb.height || fb.width > 8192 || fb.height > 8192 || fb.pitch < fb.width*4 ||
        (uint64_t)fb.pitch*fb.height > fb.buffer_bytes || fb.buffer_bytes > 128u*1024*1024) return -1;
    s->im = (struct image){fb.width,fb.height,fb.pitch,av_mallocz(fb.buffer_bytes+AV_INPUT_BUFFER_PADDING_SIZE)};
    return s->im.pixels ? 0 : -1;
}

int screen_next(struct screen *s) {
    size_t bytes = (size_t)s->im.stride * s->im.h;
    if (s->wayland) {
        if (!s->first && write(s->fd,"F",1) != 1) return -1;
        s->first = 0;
        return receive(s->fd,s->im.pixels,bytes);
    }
    struct syn_fb_display_v1 fb = {.version=1};
    if (ioctl(s->fd,SYN_FB_DISPLAY_QUERY_V1,&fb) < 0 || fb.width != (unsigned)s->im.w ||
        fb.height != (unsigned)s->im.h || fb.pitch != (unsigned)s->im.stride) return -1;
    for (size_t off = 0; off < bytes;) {
        ssize_t n = pread(s->fd,s->im.pixels+off,FFMIN(bytes-off,65536),off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

void screen_close(struct screen *s) {
    if (s->fd >= 0) close(s->fd);
    reap(s->child);
    av_freep(&s->im.pixels);
}

int audio_open(struct audio_input *a, const char *source) {
    if (!strcmp(source,"system") || !strcmp(source,"mic")) {
        int p[2];
        if (pipe(p)) return -1;
        a->pid = fork();
        if (!a->pid) {
            close(p[0]);
            if (dup2(p[1],1) < 0) _exit(126);
            close(p[1]);
            execl("/usr/bin/parec","parec","--raw","--format=s16le","--rate=48000","--channels=2",
                "--latency-msec=40", !strcmp(source,"mic") ? "--device=@DEFAULT_SOURCE@" : "--device=@DEFAULT_MONITOR@",
                (char *)NULL);
            _exit(127);
        }
        close(p[1]);
        a->fd = p[0];
        if (a->pid < 0) return -1;
    } else {
        a->fd = open(source,O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (a->fd < 0) return -1;
    }
    fcntl(a->fd,F_SETFD,FD_CLOEXEC);
    return fcntl(a->fd,F_SETFL,O_NONBLOCK);
}

int audio_poll(struct audio_input *a) {
    if (a->fd < 0 || a->used == sizeof(a->data)) return 0;
    ssize_t n = read(a->fd,a->data+a->used,sizeof(a->data)-a->used);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if (!n) return a->pid > 0 || !a->received ? -1 : 0;
    a->used += n;
    a->received = 1;
    return 0;
}

void audio_mix(struct audio_input *a, struct audio_input *b, int16_t *out) {
    size_t count[2] = {a->used / 4, b->used / 4};
    for (int i = 0; i < 1024; i++) for (int ch = 0; ch < 2; ch++) {
        int16_t v[2] = {0,0};
        if ((size_t)i < count[0]) memcpy(v,a->data+4*i+2*ch,2);
        if ((size_t)i < count[1]) memcpy(v+1,b->data+4*i+2*ch,2);
        out[2*i+ch] = b->fd < 0 ? v[0] : ((int)v[0]+v[1])/2;
    }
    struct audio_input *inputs[] = {a,b};
    for (int k = 0; k < 2; k++) {
        size_t used = FFMIN(count[k],1024)*4;
        inputs[k]->used -= used;
        memmove(inputs[k]->data,inputs[k]->data+used,inputs[k]->used);
    }
}

void audio_close(struct audio_input *a) {
    if (a->fd >= 0) close(a->fd);
    reap(a->pid);
}
