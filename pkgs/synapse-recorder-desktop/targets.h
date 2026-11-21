// Выходы и окна текущего композитора
#include "window-capture.h"
struct monitor { struct wl_output *output; uint32_t id; char name[128]; int w,h; };
static struct monitor monitors[64];
static struct synapse_window_capture_v1 *windows;
static int window_result;
static uint32_t window_w, window_h;
static void geometry(void *p,struct wl_output *o,int32_t x,int32_t y,int32_t a,int32_t b,
                     int32_t sub,const char *make,const char *model,int32_t tr) {
    (void)p;(void)o;(void)x;(void)y;(void)a;(void)b;(void)sub;(void)make;(void)model;(void)tr;
}
static void mode_event(void *p,struct wl_output *o,uint32_t f,int32_t w,int32_t h,int32_t hz) {
    (void)o;(void)hz; if (f & WL_OUTPUT_MODE_CURRENT) { struct monitor *m=p;m->w=w;m->h=h; }
}
static void output_done(void *p,struct wl_output *o) {(void)p;(void)o;}
static void output_scale(void *p,struct wl_output *o,int32_t s) {(void)p;(void)o;(void)s;}
static void output_name(void *p,struct wl_output *o,const char *name) {
    (void)o; snprintf(((struct monitor *)p)->name,sizeof(monitors[0].name),"%s",name);
}
static void description(void *p,struct wl_output *o,const char *desc) {(void)p;(void)o;(void)desc;}
static void add_monitor(struct wl_registry *r,uint32_t id,uint32_t ver) {
    static const struct wl_output_listener events={geometry,mode_event,output_done,output_scale,output_name,description};
    for (unsigned i=0;i<64;i++) if (!monitors[i].output) {
        struct monitor *m=&monitors[i];m->id=id;snprintf(m->name,sizeof(m->name),"output-%u",id);
        m->output=wl_registry_bind(r,id,&wl_output_interface,ver>4?4:ver);
        wl_output_add_listener(m->output,&events,m);break;
    }
}
static void list_window(void *p,struct synapse_window_capture_v1 *win,const char *id,const char *title) {
    (void)p;(void)win;printf("window\t%s\t",id);
    for (const unsigned char *s=(const unsigned char *)title;*s;s++) putchar(*s<32?' ':*s);
    putchar('\n');
}
static void size_window(void *p,struct synapse_window_capture_v1 *win,uint32_t w,uint32_t h) {
    (void)p;(void)win;window_w=w;window_h=h;
}
static void result_window(void *p,struct synapse_window_capture_v1 *win,uint32_t ok) {
    (void)p;(void)win;window_result=ok?1:-1;
}
static const struct synapse_window_capture_v1_listener window_events={list_window,size_window,result_window};
