// Synapse Recorder, кадры Wayland через wlr-screencopy, 21.11.2026
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "screencopy.h"
#include "targets.h"

static struct wl_display *display;
static struct wl_output *output;
static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *manager;
static struct wl_buffer *buffer;
static uint8_t *pixels;
static uint32_t width, height, pitch, format;
static size_t bytes;
static int done, flipped, first = 1;

static void global(void *data, struct wl_registry *r, uint32_t id, const char *name, uint32_t version) {
    (void)data;
    if (!strcmp(name,"wl_output")) add_monitor(r,id,version);
    if (!strcmp(name,"synapse_window_capture_v1")) {
        windows=wl_registry_bind(r,id,&synapse_window_capture_v1_interface,1);
        synapse_window_capture_v1_add_listener(windows,&window_events,NULL);
    }
    if (!strcmp(name,"wl_shm")) shm = wl_registry_bind(r,id,&wl_shm_interface,1);
    if (!strcmp(name,"zwlr_screencopy_manager_v1"))
        manager = wl_registry_bind(r,id,&zwlr_screencopy_manager_v1_interface,1);
}
static void removed(void *data, struct wl_registry *r, uint32_t id) { (void)data; (void)r;
    for (unsigned i=0;i<64;i++) if (monitors[i].id==id && monitors[i].output) {
        if (output==monitors[i].output) output=NULL;
        wl_output_destroy(monitors[i].output); monitors[i]=(struct monitor){0}; break;
    } }

static void offered(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t fmt, uint32_t w,
                    uint32_t h, uint32_t stride) {
    (void)data;
    if (!w || !h || w > 8192 || h > 8192 || stride < w*4 || (uint64_t)stride*h > 128u*1024*1024 ||
        (fmt != WL_SHM_FORMAT_XRGB8888 && fmt != WL_SHM_FORMAT_ARGB8888 &&
         fmt != WL_SHM_FORMAT_XBGR8888 && fmt != WL_SHM_FORMAT_ABGR8888)) { done = -1; return; }
    if (buffer && (width != w || height != h || pitch != stride || format != fmt)) { done = -1; return; }
    if (!buffer) {
        char path[] = "/tmp/synapse-recorder-XXXXXX";
        int fd = mkstemp(path);
        if (fd < 0) { done = -1; return; }
        unlink(path);
        width = w; height = h; pitch = stride; format = fmt; bytes = (size_t)stride*h;
        if (ftruncate(fd, bytes)) { close(fd); done = -1; return; }
        pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels != MAP_FAILED) {
            struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bytes);
            if (pool) {
                buffer = wl_shm_pool_create_buffer(pool,0,w,h,stride,fmt);
                wl_shm_pool_destroy(pool);
            }
        }
        close(fd);
        if (!buffer) { done = -1; return; }
    }
    if (frame) zwlr_screencopy_frame_v1_copy(frame, buffer);
}
static void flags(void *p, struct zwlr_screencopy_frame_v1 *f, uint32_t v) {
    (void)p; (void)f; flipped = v & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}
static void ready(void *p, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c) {
    (void)p; (void)f; (void)a; (void)b; (void)c; done = 1;
}
static void failed(void *p, struct zwlr_screencopy_frame_v1 *f) { (void)p; (void)f; done = -1; }

int main(int argc,char **argv) {
    const char *window=NULL, *monitor=NULL;
    int listing=0,cursor=1;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--list")) listing=1;
        else if (!strcmp(argv[i],"--hide-cursor")) cursor=0;
        else if (!strcmp(argv[i],"--window") && i+1<argc) window=argv[++i];
        else if (!strcmp(argv[i],"--monitor") && i+1<argc) monitor=argv[++i];
        else return 2;
    }
    display = wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry *r = wl_display_get_registry(display);
    static const struct wl_registry_listener registry = {global, removed};
    wl_registry_add_listener(r,&registry,NULL);
    if (wl_display_roundtrip(display)<0 || wl_display_roundtrip(display)<0) return 1;
    for (unsigned i=0;i<64;i++) {
        struct monitor *m=&monitors[i]; if (!m->output) continue;
        if (!output && (!monitor || !strcmp(monitor,m->name))) output=m->output;
        if (listing) printf("monitor\t%s\t%s (%dx%d)\n",m->name,m->name,m->w,m->h);
    }
    if (listing) {
        if (windows) synapse_window_capture_v1_list(windows);
        return wl_display_roundtrip(display)<0;
    }
    if (!shm || (window ? !windows : (!output || !manager))) return 1;
    if (window) {
        synapse_window_capture_v1_size(windows,window);
        if (wl_display_roundtrip(display)<0 || !window_w || window_w>8192) return 1;
        offered(NULL,NULL,WL_SHM_FORMAT_XRGB8888,window_w,window_h,window_w*4);
        if (!buffer) return 1;
    }
    static const struct zwlr_screencopy_frame_v1_listener listener = {
        .buffer=offered, .flags=flags, .ready=ready, .failed=failed
    };
    char command;
    while (read(STDIN_FILENO,&command,1) == 1) {
        done = flipped = 0;
        if (window) {
            window_result=0;
            synapse_window_capture_v1_copy(windows,window,buffer,cursor);
            if (wl_display_roundtrip(display)<0 || window_result!=1) return 1;
        } else {
            if (!output) return 1;
            struct zwlr_screencopy_frame_v1 *f=zwlr_screencopy_manager_v1_capture_output(manager,cursor,output);
            if (!f) return 1;
            zwlr_screencopy_frame_v1_add_listener(f,&listener,NULL);
            while (!done && wl_display_dispatch(display)>=0) {}
            zwlr_screencopy_frame_v1_destroy(f);
            if (done!=1) return 1;
        }
        if (first) {
            uint32_t geometry[] = {width,height};
            if (fwrite(geometry,sizeof(geometry),1,stdout) != 1) return 1;
            first = 0;
        }
        for (unsigned y = 0; y < height; y++) {
            uint8_t *line = pixels + (flipped ? height-1-y : y)*pitch;
            if (format == WL_SHM_FORMAT_XBGR8888 || format == WL_SHM_FORMAT_ABGR8888)
                for (unsigned x = 0; x < width; x++) {
                    uint8_t tmp = line[4*x]; line[4*x] = line[4*x+2]; line[4*x+2] = tmp;
                }
            if (fwrite(line,4,width,stdout) != width) return 1;
        }
        if (fflush(stdout)) return 1;
    }
    if (buffer) wl_buffer_destroy(buffer);
    if (pixels && pixels != MAP_FAILED) munmap(pixels,bytes);
    wl_display_disconnect(display);
    return 0;
}
