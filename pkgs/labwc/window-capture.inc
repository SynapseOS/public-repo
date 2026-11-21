// SynapseOS: снимок дерева поверхностей выбранного окна
#include "synapse-window-server.h"
#include "synapse-window-wire.inc"
#include "session-lock.h"
#include <drm_fourcc.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/transform.h>

static struct view *capture_view(struct server *s, const char *id) {
    if (!s->foreign_toplevel_list || (s->session_lock_manager && s->session_lock_manager->locked)) return NULL;
    struct wlr_ext_foreign_toplevel_handle_v1 *h;
    wl_list_for_each(h, &s->foreign_toplevel_list->toplevels, link) {
        struct view *v = h->data;
        if (v && v->mapped && v->surface && !strcmp(h->identifier, id)) return v;
    }
    return NULL;
}

static void capture_list(struct wl_client *client, struct wl_resource *r) {
    (void)client;
    struct server *s = wl_resource_get_user_data(r);
    if (!s->foreign_toplevel_list || (s->session_lock_manager && s->session_lock_manager->locked)) return;
    struct wlr_ext_foreign_toplevel_handle_v1 *h;
    wl_list_for_each(h, &s->foreign_toplevel_list->toplevels, link) {
        struct view *v = h->data;
        if (v && v->mapped && v->surface) synapse_window_capture_v1_send_window(r,h->identifier,
            h->title && *h->title ? h->title : (h->app_id ? h->app_id : "Window"));
    }
}

static void capture_size(struct wl_client *client, struct wl_resource *r, const char *id) {
    (void)client;
    struct view *v = capture_view(wl_resource_get_user_data(r),id);
    if (v) synapse_window_capture_v1_send_geometry(r,v->surface->current.width,v->surface->current.height);
}

struct capture_draw {
    struct wlr_render_pass *pass;
    double scale;
    int x, y;
    struct wlr_surface *focused;
    int focus_x, focus_y;
    bool has_focus;
};

static void capture_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
    struct capture_draw *d = data;
    if (surface==d->focused) { d->has_focus=true; d->focus_x=sx; d->focus_y=sy; }
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture) return;
    struct wlr_render_texture_options opts = {
        .texture = texture,
        .dst_box = {d->x + (int)(sx*d->scale), d->y + (int)(sy*d->scale),
                    (int)(surface->current.width*d->scale), (int)(surface->current.height*d->scale)},
        .transform = wlr_output_transform_invert(surface->current.transform),
    };
    if (!opts.dst_box.width || !opts.dst_box.height) return;
    wlr_surface_get_buffer_source_box(surface,&opts.src_box);
    wlr_render_pass_add_texture(d->pass,&opts);
}

static void capture_copy(struct wl_client *client, struct wl_resource *r, const char *id, struct wl_resource *target, uint32_t include_cursor) {
    (void)client;
    struct server *s = wl_resource_get_user_data(r);
    struct view *v = capture_view(s,id);
    bool ok = false;
    struct wlr_buffer *buf = wlr_buffer_try_from_resource(target);
    struct wlr_shm_attributes shm;
    if (buf && v && v->surface->current.width > 0 && v->surface->current.height > 0 &&
        buf->width > 0 && buf->height > 0 && (int64_t)buf->width*buf->height <= 33554432 &&
        wlr_buffer_get_shm(buf,&shm) && shm.format == DRM_FORMAT_XRGB8888) {
        struct capture_draw d = {.pass=wlr_renderer_begin_buffer_pass(s->renderer,buf,NULL)};
        if (d.pass) {
            int w = v->surface->current.width, h = v->surface->current.height;
            d.scale = (double)buf->width/w < (double)buf->height/h ? (double)buf->width/w : (double)buf->height/h;
            d.x = (buf->width-(int)(w*d.scale))/2;
            d.y = (buf->height-(int)(h*d.scale))/2;
            wlr_render_pass_add_rect(d.pass,&(struct wlr_render_rect_options){
                .color={0,0,0,1}, .blend_mode=WLR_RENDER_BLEND_MODE_NONE});
            struct wlr_seat_pointer_state *pointer=&s->seat.seat->pointer_state;
            if (include_cursor && s->seat.cursor_visible) d.focused=pointer->focused_surface;
            wlr_surface_for_each_surface(v->surface,capture_surface,&d);
            if (d.has_focus) {
                struct wlr_output *out=wlr_output_layout_output_at(s->output_layout,
                    s->seat.cursor->x,s->seat.cursor->y);
                struct wlr_output_cursor *cursor;
                if (out) wl_list_for_each(cursor,&out->cursors,link) {
                    if (!cursor->enabled || !cursor->visible || !cursor->texture) continue;
                    double scale=d.scale/out->scale;
                    struct wlr_render_texture_options opts={
                        .texture=cursor->texture,.src_box=cursor->src_box,
                        .transform=wlr_output_transform_invert(cursor->transform),
                        .dst_box={d.x+(int)((d.focus_x+pointer->sx)*d.scale-cursor->hotspot_x*scale),
                                  d.y+(int)((d.focus_y+pointer->sy)*d.scale-cursor->hotspot_y*scale),
                                  (int)(cursor->width*scale),(int)(cursor->height*scale)}
                    };
                    if (opts.dst_box.width && opts.dst_box.height) wlr_render_pass_add_texture(d.pass,&opts);
                    break;
                }
            }
            ok = wlr_render_pass_submit(d.pass);
        }
    }
    if (buf) wlr_buffer_unlock(buf);
    synapse_window_capture_v1_send_result(r,ok);
}

static void capture_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    static const struct synapse_window_capture_v1_interface ops = {capture_list,capture_size,capture_copy};
    struct wl_resource *r = wl_resource_create(client,&synapse_window_capture_v1_interface,version,id);
    if (r) wl_resource_set_implementation(r,&ops,data,NULL);
    else wl_client_post_no_memory(client);
}
