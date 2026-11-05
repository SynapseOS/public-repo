// synapse-headless.inc: Вывод кадров wlroots в framebuffer SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl, SynAPI и wlroots 0.19
// Описание: Подключить headless output к устройству /dev/fb0
// Подсистема: графическая оболочка
// Первое изменение: 28.10.2026
// Последнее изменение: 30.10.2026
// Доступ: общедоступный

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <synapse/gpu.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>

#include "lumine_fb.h"

static int synapse_fb_fd = -1;
static uint8_t *synapse_fb_pixels;
static uint8_t *synapse_fb_readback_pixels;
static size_t synapse_fb_readback_capacity;
static size_t synapse_fb_bytes;
static uint32_t synapse_fb_width;
static uint32_t synapse_fb_height;
static uint32_t synapse_fb_stride;
static uint32_t synapse_fb_initial_width;
static uint32_t synapse_fb_initial_height;
static uint32_t synapse_fb_attempt_width;
static uint32_t synapse_fb_attempt_height;
static bool synapse_fb_modeset;
static bool synapse_fb_force_full = true;
static bool synapse_fb_full_copy;
static bool synapse_fb_metrics;
static bool synapse_fb_buffer_reported;
static bool synapse_fb_direct_scanout;
static struct wlr_buffer *synapse_fb_scanout_buffer;
static uint64_t synapse_fb_metric_frames;
static uint64_t synapse_fb_metric_bytes;
static uint64_t synapse_fb_metric_copy_ns;
static uint64_t synapse_fb_metric_flush_ns;
static struct timespec synapse_fb_metric_started;
static struct wl_event_source *synapse_fb_mode_timer;

struct synapse_fb_dirty_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

// Вернуть время в наносекундах для измерения задержки вывода
static uint64_t synapse_fb_nanoseconds(const struct timespec *time) {
    return (uint64_t)time->tv_sec * 1000000000ULL + (uint64_t)time->tv_nsec;
}

// Напечатать счётчики вывода при включённом профилировании
static void synapse_fb_report(uint64_t bytes, uint64_t copy_ns, uint64_t flush_ns) {
    if (!synapse_fb_metrics) {
        return;
    }

    synapse_fb_metric_frames++;
    synapse_fb_metric_bytes += bytes;
    synapse_fb_metric_copy_ns += copy_ns;
    synapse_fb_metric_flush_ns += flush_ns;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }

    uint64_t elapsed = synapse_fb_nanoseconds(&now) -
        synapse_fb_nanoseconds(&synapse_fb_metric_started);
    if (elapsed >= 1000000000ULL) {
        fprintf(stderr,
            "SYNAPSE_FB frames=%llu copied_bytes=%llu copy_ms=%.3f flush_ms=%.3f elapsed_ms=%.3f\n",
            (unsigned long long)synapse_fb_metric_frames,
            (unsigned long long)synapse_fb_metric_bytes,
            synapse_fb_metric_copy_ns / 1e6, synapse_fb_metric_flush_ns / 1e6,
            elapsed / 1e6);
        synapse_fb_metric_frames = 0;
        synapse_fb_metric_bytes = 0;
        synapse_fb_metric_copy_ns = 0;
        synapse_fb_metric_flush_ns = 0;
        synapse_fb_metric_started = now;
    }
}

// Открыть framebuffer и передать его размер выходу wlroots
static bool synapse_fb_open(unsigned int *width, unsigned int *height) {
    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;

    synapse_fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (synapse_fb_fd < 0) {
        return false;
    }

    if (ioctl(synapse_fb_fd, 0x4600, &variable) != 0 ||
        ioctl(synapse_fb_fd, 0x4602, &fixed) != 0 ||
        variable.bits_per_pixel != 32 || variable.red.offset != 16 ||
        variable.green.offset != 8 || variable.blue.offset != 0 ||
        variable.xres == 0 || variable.yres == 0 ||
        variable.xres > 16384 || variable.yres > 16384 ||
        fixed.line_length < variable.xres * 4U || fixed.line_length > 262144U ||
        variable.yres > SIZE_MAX / fixed.line_length) {
        goto fail;
    }

    synapse_fb_bytes = (size_t)fixed.line_length * variable.yres;
    void *pixels = mmap(NULL, synapse_fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
        synapse_fb_fd, 0);
    if (pixels == MAP_FAILED) {
        goto fail;
    }

    synapse_fb_pixels = pixels;
    if (ioctl(synapse_fb_fd, 0x4626, 0) != 0) {
        munmap(synapse_fb_pixels, synapse_fb_bytes);
        synapse_fb_pixels = NULL;
        goto fail;
    }

    synapse_fb_width = variable.xres;
    synapse_fb_height = variable.yres;
    synapse_fb_stride = fixed.line_length;
    synapse_fb_initial_width = variable.xres;
    synapse_fb_initial_height = variable.yres;
    synapse_fb_attempt_width = variable.xres;
    synapse_fb_attempt_height = variable.yres;
    synapse_fb_force_full = true;
    synapse_fb_full_copy = getenv("SYNAPSE_FB_FORCE_FULL") != NULL;
    synapse_fb_metrics = getenv("SYNAPSE_FB_METRICS") != NULL;
    const char *direct_scanout = getenv("SYNAPSE_FB_DIRECT_SCANOUT");
    synapse_fb_direct_scanout = direct_scanout != NULL && strcmp(direct_scanout, "0") != 0;
    synapse_fb_buffer_reported = false;
    if (synapse_fb_metrics &&
        clock_gettime(CLOCK_MONOTONIC, &synapse_fb_metric_started) != 0) {
        synapse_fb_metrics = false;
    }
    struct syn_fb_display_v1 display = {.version = 1};
    synapse_fb_modeset = ioctl(synapse_fb_fd, SYN_FB_DISPLAY_QUERY_V1, &display) == 0 &&
        (display.capabilities & SYN_FB_MODESET) != 0;
    *width = variable.xres;
    *height = variable.yres;
    return true;

fail:
    close(synapse_fb_fd);
    synapse_fb_fd = -1;
    return false;
}

// Освободить framebuffer и вернуть экран консоли
static void synapse_fb_close(void) {
    if (synapse_fb_mode_timer != NULL) {
        wl_event_source_remove(synapse_fb_mode_timer);
        synapse_fb_mode_timer = NULL;
    }

    if (synapse_fb_fd < 0) {
        return;
    }

    free(synapse_fb_readback_pixels);
    synapse_fb_readback_pixels = NULL;
    synapse_fb_readback_capacity = 0;

    ioctl(synapse_fb_fd, SYN_FB_SCANOUT_RESTORE_V1, 0);
    if (synapse_fb_scanout_buffer != NULL) {
        wlr_buffer_unlock(synapse_fb_scanout_buffer);
        synapse_fb_scanout_buffer = NULL;
    }

    ioctl(synapse_fb_fd, 0x4627, 0);
    munmap(synapse_fb_pixels, synapse_fb_bytes);
    close(synapse_fb_fd);
    synapse_fb_pixels = NULL;
    synapse_fb_fd = -1;
}

// Прочитать запрошенное разрешение из общих настроек системы
static bool synapse_fb_read_mode(uint32_t *width, uint32_t *height) {
    FILE *file = fopen("/etc/settings.ini", "r");
    if (file == NULL) {
        return false;
    }

    char line[256];
    bool display_section = false;
    bool found = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = line;
        while (*text == ' ' || *text == '\t') {
            text++;
        }

        if (*text == '[') {
            display_section = strncmp(text, "[Display]", 9) == 0;
            continue;
        }

        if (!display_section || strncmp(text, "Resolution", 10) != 0) {
            continue;
        }

        char *value = strchr(text + 10, '=');
        if (value == NULL) {
            continue;
        }

        value++;
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        if (strncmp(value, "auto", 4) == 0 &&
            (value[4] == '\r' || value[4] == '\n' || value[4] == 0)) {
            *width = synapse_fb_initial_width;
            *height = synapse_fb_initial_height;
            found = true;
        } else {
            unsigned requested_width;
            unsigned requested_height;
            char extra;
            if (sscanf(value, "%ux%u %c", &requested_width, &requested_height, &extra) == 2 &&
                requested_width >= 320 && requested_height >= 200 &&
                requested_width <= 3840 && requested_height <= 2160) {
                *width = requested_width;
                *height = requested_height;
                found = true;
            }
        }

        break;
    }

    fclose(file);
    return found;
}

// Восстановить старый режим после неудачной смены разрешения
static bool synapse_fb_restore(uint32_t width, uint32_t height, size_t bytes) {
    struct syn_fb_mode_v1 mode = {
        .version = 1,
        .width = width,
        .height = height,
        .format = SYN_FB_FORMAT_XRGB8888,
    };

    if (ioctl(synapse_fb_fd, SYN_FB_MODE_SET_V1, &mode) != 0) {
        return false;
    }

    void *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, synapse_fb_fd, 0);
    if (pixels == MAP_FAILED) {
        return false;
    }

    synapse_fb_pixels = pixels;
    synapse_fb_bytes = bytes;
    synapse_fb_width = width;
    synapse_fb_height = height;
    synapse_fb_stride = width * 4U;
    synapse_fb_force_full = true;
    return true;
}

// Согласовать режим framebuffer с размером выхода Wayland
static bool synapse_fb_set_mode(struct wlr_output *output, uint32_t width, uint32_t height) {
    uint32_t old_width = synapse_fb_width;
    uint32_t old_height = synapse_fb_height;
    size_t old_bytes = synapse_fb_bytes;

    if (synapse_fb_scanout_buffer != NULL) {
        if (ioctl(synapse_fb_fd, SYN_FB_SCANOUT_RESTORE_V1, 0) != 0) {
            return false;
        }

        wlr_buffer_unlock(synapse_fb_scanout_buffer);
        synapse_fb_scanout_buffer = NULL;
    }

    if (munmap(synapse_fb_pixels, old_bytes) != 0) {
        return false;
    }

    synapse_fb_pixels = NULL;
    struct syn_fb_mode_v1 mode = {
        .version = 1,
        .width = width,
        .height = height,
        .format = SYN_FB_FORMAT_XRGB8888,
    };
    if (ioctl(synapse_fb_fd, SYN_FB_MODE_SET_V1, &mode) != 0) {
        synapse_fb_restore(old_width, old_height, old_bytes);
        return false;
    }

    struct syn_fb_display_v1 display = {.version = 1};
    if (ioctl(synapse_fb_fd, SYN_FB_DISPLAY_QUERY_V1, &display) != 0 ||
        display.width != width || display.height != height ||
        display.pitch != width * 4U ||
        display.buffer_bytes != (uint64_t)display.pitch * display.height) {
        synapse_fb_restore(old_width, old_height, old_bytes);
        return false;
    }

    void *pixels = mmap(NULL, display.buffer_bytes, PROT_READ | PROT_WRITE,
        MAP_SHARED, synapse_fb_fd, 0);
    if (pixels == MAP_FAILED) {
        synapse_fb_restore(old_width, old_height, old_bytes);
        return false;
    }

    synapse_fb_pixels = pixels;
    synapse_fb_bytes = display.buffer_bytes;
    synapse_fb_width = width;
    synapse_fb_height = height;
    synapse_fb_stride = display.pitch;
    synapse_fb_force_full = true;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, width, height, 0);
    bool committed = wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);
    if (!committed) {
        munmap(synapse_fb_pixels, synapse_fb_bytes);
        synapse_fb_pixels = NULL;
        synapse_fb_restore(old_width, old_height, old_bytes);
        return false;
    }

    wlr_output_schedule_frame(output);
    return true;
}

// Проверять настройки разрешения во время работы композитора
static int synapse_fb_poll_mode(void *data) {
    struct wlr_output *output = data;
    uint32_t width = 0;
    uint32_t height = 0;

    if (synapse_fb_modeset && output->enabled &&
        synapse_fb_read_mode(&width, &height) &&
        (width != synapse_fb_attempt_width || height != synapse_fb_attempt_height)) {
        synapse_fb_attempt_width = width;
        synapse_fb_attempt_height = height;
        if (width != synapse_fb_width || height != synapse_fb_height) {
            if (!synapse_fb_set_mode(output, width, height)) {
                wlr_log(WLR_ERROR, "Synapse display mode %ux%u failed: %s",
                    width, height, strerror(errno));
            }
        }
    }

    wl_event_source_timer_update(synapse_fb_mode_timer, 500);
    return 0;
}

// Вернуть программный экран, когда готовый GPU-буфер не подходит для прямого вывода
static bool synapse_fb_restore_scanout(void) {
    if (synapse_fb_scanout_buffer == NULL) {
        return true;
    }

    if (ioctl(synapse_fb_fd, SYN_FB_SCANOUT_RESTORE_V1, 0) != 0) {
        return false;
    }

    wlr_buffer_unlock(synapse_fb_scanout_buffer);
    synapse_fb_scanout_buffer = NULL;
    synapse_fb_force_full = true;
    return true;
}

// Показать линейный GPU-буфер, если его проверило ядро
// Вернуть -1, если после ошибки нельзя безопасно продолжить программный вывод
static int synapse_fb_try_direct_scanout(struct wlr_buffer *buffer) {
    if (!synapse_fb_direct_scanout || buffer == NULL) {
        return 0;
    }

    struct wlr_dmabuf_attributes dmabuf = {0};
    if (!wlr_buffer_get_dmabuf(buffer, &dmabuf) || dmabuf.n_planes != 1 || dmabuf.fd[0] < 0 ||
        dmabuf.width != (int32_t)synapse_fb_width ||
        dmabuf.height != (int32_t)synapse_fb_height ||
        dmabuf.format != DRM_FORMAT_XRGB8888 ||
        (dmabuf.modifier != DRM_FORMAT_MOD_LINEAR &&
         dmabuf.modifier != DRM_FORMAT_MOD_INVALID) ||
        dmabuf.offset[0] != 0 || dmabuf.stride[0] < synapse_fb_width * 4U) {
        return 0;
    }

    // PRIME обозначает ресурс VirtIO GPU, его размещение проверяет устройство
    struct syn_fb_scanout_check_v1 request = {
        .version = 1,
        .width = synapse_fb_width,
        .height = synapse_fb_height,
        .format = SYN_FB_FORMAT_XRGB8888,
        .prime_fd = dmabuf.fd[0],
        .stride = dmabuf.stride[0],
    };

    if (ioctl(synapse_fb_fd, SYN_FB_SCANOUT_CHECK_V1, &request) != 0) {
        if (synapse_fb_metrics && !synapse_fb_buffer_reported) {
            fprintf(stderr, "SYNAPSE_FB scanout_check_failed errno=%d\n", errno);
        }

        return 0;
    }

    if (ioctl(synapse_fb_fd, SYN_FB_SCANOUT_PRESENT_V1, &request) != 0) {
        if (synapse_fb_metrics && !synapse_fb_buffer_reported) {
            fprintf(stderr, "SYNAPSE_FB scanout_present_failed errno=%d\n", errno);
        }

        return ioctl(synapse_fb_fd, SYN_FB_SCANOUT_RESTORE_V1, 0) == 0 ? 0 : -1;
    }

    if (synapse_fb_scanout_buffer != buffer) {
        struct wlr_buffer *previous = synapse_fb_scanout_buffer;
        synapse_fb_scanout_buffer = wlr_buffer_lock(buffer);
        if (previous != NULL) {
            wlr_buffer_unlock(previous);
        }
    }

    synapse_fb_report(0, 0, 0);
    return 1;
}

// Прочитать изменившиеся области из буфера видеокарты
static bool synapse_fb_readback(struct wlr_renderer *renderer, struct wlr_buffer *buffer,
    const pixman_box32_t *boxes, int count, bool full) {
    if (renderer == NULL || buffer == NULL) {
        return false;
    }

    struct wlr_texture *texture = wlr_texture_from_buffer(renderer, buffer);
    if (texture == NULL) {
        return false;
    }

    int rectangles = full ? 1 : count;
    bool success = true;
    for (int index = 0; index < rectangles; index++) {
        int32_t x1 = full ? 0 : boxes[index].x1;
        int32_t y1 = full ? 0 : boxes[index].y1;
        int32_t x2 = full ? (int32_t)synapse_fb_width : boxes[index].x2;
        int32_t y2 = full ? (int32_t)synapse_fb_height : boxes[index].y2;
        size_t row_bytes = (size_t)(x2 - x1) * 4U;
        size_t rows = (size_t)(y2 - y1);
        bool direct = x1 == 0 && synapse_fb_stride == row_bytes;

        if (!direct) {
            if (rows > SIZE_MAX / row_bytes) {
                success = false;
                break;
            }

            size_t needed = row_bytes * rows;
            if (needed > synapse_fb_readback_capacity) {
                void *next = realloc(synapse_fb_readback_pixels, needed);
                if (next == NULL) {
                    success = false;
                    break;
                }

                synapse_fb_readback_pixels = next;
                synapse_fb_readback_capacity = needed;
            }
        }

        struct wlr_texture_read_pixels_options options = {
            .data = direct ? synapse_fb_pixels : synapse_fb_readback_pixels,
            .format = DRM_FORMAT_XRGB8888,
            .stride = row_bytes,
            .dst_x = 0,
            .dst_y = direct ? (uint32_t)y1 : 0,
            .src_box = {x1, y1, x2 - x1, y2 - y1},
        };

        if (!wlr_texture_read_pixels(texture, &options)) {
            success = false;
            break;
        }

        if (!direct) {
            for (size_t row = 0; row < rows; row++) {
                memcpy(synapse_fb_pixels + ((size_t)y1 + row) * synapse_fb_stride +
                    (size_t)x1 * 4U, synapse_fb_readback_pixels + row * row_bytes, row_bytes);
            }
        }
    }

    wlr_texture_destroy(texture);
    return success;
}

// Перенести готовый буфер в framebuffer и показать кадр
static bool synapse_fb_present(struct wlr_output *output, const struct wlr_output_state *state) {
    void *pixels;
    uint32_t format;
    size_t stride;
    if (output == NULL || state == NULL || synapse_fb_fd < 0 || synapse_fb_pixels == NULL ||
        state->buffer == NULL) {
        return false;
    }

    struct wlr_buffer *buffer = state->buffer;

    if (buffer->width != (int)synapse_fb_width ||
        buffer->height != (int)synapse_fb_height) {
        return false;
    }

    int scanout = synapse_fb_try_direct_scanout(buffer);
    if (scanout > 0) {
        return true;
    }

    if (scanout < 0) {
        return false;
    }

    if (!synapse_fb_restore_scanout()) {
        return false;
    }

    if (synapse_fb_metrics && !synapse_fb_buffer_reported) {
        struct wlr_dmabuf_attributes dmabuf = {0};
        bool exported = wlr_buffer_get_dmabuf(buffer, &dmabuf);
        bool candidate = exported && dmabuf.n_planes == 1 && dmabuf.fd[0] >= 0 &&
            dmabuf.width == (int32_t)synapse_fb_width &&
            dmabuf.height == (int32_t)synapse_fb_height && dmabuf.offset[0] == 0 &&
            dmabuf.stride[0] >= synapse_fb_width * 4U &&
            (dmabuf.format == DRM_FORMAT_XRGB8888 || dmabuf.format == DRM_FORMAT_ARGB8888);

        if (exported && dmabuf.n_planes > 0) {
            fprintf(stderr,
                "SYNAPSE_FB dmabuf=1 scanout_candidate=%u planes=%d format=0x%08x "
                "size=%dx%d stride=%u offset=%u modifier=0x%llx\n",
                candidate ? 1U : 0U, dmabuf.n_planes, dmabuf.format,
                dmabuf.width, dmabuf.height, dmabuf.stride[0], dmabuf.offset[0],
                (unsigned long long)dmabuf.modifier);
        } else {
            fprintf(stderr, "SYNAPSE_FB dmabuf=0 scanout_candidate=0\n");
        }

        synapse_fb_buffer_reported = true;
    }

    pixman_box32_t *boxes = NULL;
    int count = 0;
    bool full = synapse_fb_force_full || synapse_fb_full_copy ||
        (state->committed & WLR_OUTPUT_STATE_DAMAGE) == 0;
    if (!full) {
        boxes = pixman_region32_rectangles((pixman_region32_t *)&state->damage, &count);
        if (count < 0 || count > 16) {
            full = true;
        } else if (count == 0) {
            synapse_fb_report(0, 0, 0);
            return true;
        }

        uint64_t damaged_pixels = 0;
        for (int index = 0; !full && index < count; index++) {
            if (boxes[index].x1 < 0 || boxes[index].y1 < 0 ||
                boxes[index].x2 > (int32_t)synapse_fb_width ||
                boxes[index].y2 > (int32_t)synapse_fb_height ||
                boxes[index].x2 <= boxes[index].x1 ||
                boxes[index].y2 <= boxes[index].y1) {
                full = true;
                break;
            }

            damaged_pixels += (uint64_t)(boxes[index].x2 - boxes[index].x1) *
                (uint64_t)(boxes[index].y2 - boxes[index].y1);
        }

        if (damaged_pixels * 2U >= (uint64_t)synapse_fb_width * synapse_fb_height) {
            full = true;
        }
    }

    bool direct = wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
        &pixels, &format, &stride);
    bool valid = !direct ||
        ((format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888) &&
            stride >= (size_t)synapse_fb_width * 4U);
    struct timespec copy_started = {0};
    if (synapse_fb_metrics) {
        clock_gettime(CLOCK_MONOTONIC, &copy_started);
    }

    uint64_t bytes_copied = 0;
    if (valid && direct) {
        int rectangles = full ? 1 : count;
        for (int index = 0; index < rectangles; index++) {
            int32_t x1 = full ? 0 : boxes[index].x1;
            int32_t y1 = full ? 0 : boxes[index].y1;
            int32_t x2 = full ? (int32_t)synapse_fb_width : boxes[index].x2;
            int32_t y2 = full ? (int32_t)synapse_fb_height : boxes[index].y2;
            size_t row_bytes = (size_t)(x2 - x1) * 4U;
            for (int32_t row = y1; row < y2; row++) {
                memcpy(synapse_fb_pixels + (size_t)row * synapse_fb_stride + (size_t)x1 * 4U,
                    (uint8_t *)pixels + (size_t)row * stride + (size_t)x1 * 4U, row_bytes);
            }

            bytes_copied += row_bytes * (size_t)(y2 - y1);
        }
    }

    if (direct) {
        wlr_buffer_end_data_ptr_access(buffer);
    } else {
        valid = synapse_fb_readback(output->renderer, buffer, boxes, count, full);
        if (valid) {
            int rectangles = full ? 1 : count;
            for (int index = 0; index < rectangles; index++) {
                uint32_t width = full ? synapse_fb_width :
                    (uint32_t)(boxes[index].x2 - boxes[index].x1);
                uint32_t height = full ? synapse_fb_height :
                    (uint32_t)(boxes[index].y2 - boxes[index].y1);
                bytes_copied += (uint64_t)width * height * 4U;
            }
        }
    }
    if (!valid) {
        synapse_fb_force_full = true;
        return false;
    }

    struct timespec copy_finished = {0};
    if (synapse_fb_metrics) {
        clock_gettime(CLOCK_MONOTONIC, &copy_finished);
    }

    int rectangles = full ? 1 : count;
    for (int index = 0; index < rectangles; index++) {
        struct synapse_fb_dirty_rect rectangle = {
            .x = full ? 0U : (uint32_t)boxes[index].x1,
            .y = full ? 0U : (uint32_t)boxes[index].y1,
            .width = full ? synapse_fb_width : (uint32_t)(boxes[index].x2 - boxes[index].x1),
            .height = full ? synapse_fb_height : (uint32_t)(boxes[index].y2 - boxes[index].y1),
        };
        if (ioctl(synapse_fb_fd, 0x4621, &rectangle) != 0) {
            synapse_fb_force_full = true;
            return false;
        }
    }

    if (ioctl(synapse_fb_fd, 0x4622, 0) != 0) {
        synapse_fb_force_full = true;
        return false;
    }

    synapse_fb_force_full = false;
    if (synapse_fb_metrics) {
        struct timespec flushed;
        if (clock_gettime(CLOCK_MONOTONIC, &flushed) == 0) {
            synapse_fb_report(bytes_copied,
                synapse_fb_nanoseconds(&copy_finished) - synapse_fb_nanoseconds(&copy_started),
                synapse_fb_nanoseconds(&flushed) - synapse_fb_nanoseconds(&copy_finished));
        }
    }

    return true;
}
