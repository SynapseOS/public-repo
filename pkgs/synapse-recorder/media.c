// SynapseOS, просмотр камеры и запись звука PCM
#define _GNU_SOURCE
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void finish(int sig) { (void)sig; stopped = 1; }

static int send_bytes(int fd, const void *ptr, size_t size) {
    const uint8_t *p = ptr;
    while (size) {
        ssize_t n = write(fd,p,size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= n;
    }
    return 0;
}

// Отложенный вывод миниатюры сохраняет границы кадра даже при заполненном канале GUI
static int flush_preview(const uint8_t *packet, size_t size, size_t *sent) {
    while (*sent < size) {
        ssize_t n = write(STDOUT_FILENO, packet + *sent, size - *sent);
        if (n > 0) {
            *sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }
    return 0;
}

// Один запрос GUI получает целую миниатюру, запись видео продолжается независимо от скорости чтения
int preview_recorded_frame(const struct image *view) {
    static uint8_t packet[8 + 320 * 240 * 4];
    static size_t size, sent;
    static int configured;
    if (!configured) {
        int flags = fcntl(STDOUT_FILENO, F_GETFL);
        if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
            return -1;
        }
        configured = 1;
    }
    if (sent < size) {
        return flush_preview(packet, size, &sent);
    }
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    char request;
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN) || read(p.fd, &request, 1) != 1 || request != 'F') {
        return 0;
    }
    uint32_t w = FFMIN(320, view->w), h = FFMAX(1, (int64_t)w * view->h / view->w);
    if (h > 240) {
        h = 240;
        w = FFMAX(1, (int64_t)h * view->w / view->h);
    }
    memcpy(packet, &w, 4);
    memcpy(packet + 4, &h, 4);
    size = 8;
    sent = 0;
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = view->pixels + (int64_t)y * view->h / h * view->stride;
        for (unsigned x = 0; x < w; x++) {
            memcpy(packet + size, row + (int64_t)x * view->w / w * 4, 4);
            size += 4;
        }
    }
    return flush_preview(packet, size, &sent);
}

int camera_preview(const char *device) {
    struct camera camera = {.fd = -1};
    struct image view = {0};
    int rc = 1;
    signal(SIGPIPE,SIG_IGN);
    if (camera_open(&camera, device, CAMERA_PREVIEW_MODE) < 0) goto out;
    int width = FFMIN(camera.w,640);
    if (allocate_image(&view,width,FFMAX(1,(int64_t)width*camera.h/camera.w)) < 0) goto out;
    uint32_t dimensions[] = {view.w,view.h};
    char req;
    while (read(0,&req,1) == 1 && req == 'F') {
        if (camera_read(&camera,&view) < 0 || send_bytes(1,dimensions,sizeof(dimensions)) < 0) goto out;
        if (send_bytes(1, view.pixels, (size_t)view.stride * view.h) < 0) {
            goto out;
        }
    }
    rc = 0;
out:
    if (rc) fputs("Не удалось получить кадр с камеры\n",stderr);
    camera_close(&camera);
    av_free(view.pixels);
    return rc;
}

static void le32(uint8_t *p, uint32_t n) {
    for (unsigned i=0; i<4; i++) p[i]=(uint8_t)(n>>(8*i));
}

int voice_record(const char *path, const char *source) {
    struct audio_input input = {.fd = -1};
    uint8_t header[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\1\0\2";
    uint32_t count = 0;
    int fd = -1, rc = 1;
    struct sigaction sa = {.sa_handler=finish};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM,&sa,NULL);
    sigaction(SIGINT,&sa,NULL);
    le32(header+24,48000);
    le32(header+28,192000);
    header[32]=4; header[34]=16;
    memcpy(header+36,"data",4);
    if (audio_open(&input,source) < 0) goto out;
    int64_t last=monotime();
    uint8_t block[16384];
    while (!stopped) {
        struct pollfd p={input.fd,POLLIN,0};
        int ready=poll(&p,1,250);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0 || monotime()-last>5000000) goto out;
        if (!ready) continue;
        ssize_t n=read(input.fd,block,sizeof(block));
        if (n<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if (!n && !input.pid) break;
        if (n<=0 || (uint64_t)count+n > UINT32_MAX-36) goto out;
        if (fd<0) {
            fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if (fd<0 || send_bytes(fd,header,sizeof(header))<0) goto out;
            recording_state('R'); puts("RECORDING"); fflush(stdout);
        }
        if (send_bytes(fd,block,n)<0) goto out;
        count+=(uint32_t)n;
        last=monotime();
    }
    rc = count ? 0 : 1;
out:
    recording_state(0);
    audio_close(&input);
    if (fd>=0) {
        count-=count%4;
        le32(header+4,count+36);
        le32(header+40,count);
        if (ftruncate(fd,44+(off_t)count) || lseek(fd,0,SEEK_SET)<0 ||
            send_bytes(fd,header,sizeof(header))<0) rc=1;
        if (close(fd)) rc=1;
        if (!count) unlink(path);
    }
    if (rc) fputs("Запись звука прервана: проверьте источник и место на диске\n",stderr);
    return rc;
}
