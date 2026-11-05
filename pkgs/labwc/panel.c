// panel.c: Панель окон рабочего стола Labwc
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и Wayland
// Описание: Показывать меню, работающие окна и часы на нижней панели
// Подсистема: графическая оболочка
// Первое изменение: 28.10.2026
// Последнее изменение: 30.10.2026
// Доступ: общедоступный

#define _POSIX_C_SOURCE 200809L

#include <cairo.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "foreign-toplevel.h"
#include "layer-shell.h"

struct panel_window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct panel_window *next;
    char *title;
    bool active;
    bool minimized;
};

struct panel_buffer {
    struct wl_buffer *handle;
    void *pixels;
    size_t size;
};

static struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct zwlr_layer_shell_v1 *shell;
    struct zwlr_layer_surface_v1 *layer;
    struct zwlr_layer_surface_v1 *background_layer;
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    struct wl_surface *surface;
    struct wl_surface *background_surface;
    struct panel_window *windows;
    int width;
    int height;
    int background_width;
    int background_height;
    int pointer_x;
    int last_minute;
    int theme;
    cairo_surface_t *menu_icon;
    struct stat menu_icon_stat;
    bool menu_icon_valid;
} panel;

enum {
    PANEL_MENU_WIDTH = 118,
    PANEL_FILES_WIDTH = 94,
    PANEL_TERMINAL_WIDTH = 118,
    PANEL_TASK_WIDTH = 156,
    PANEL_MENU_ICON_SIZE = 24,
};

// Прочитать активную тему для цветов нижней панели
static int read_theme(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == 0) {
        home = "/root";
    }

    char path[512];
    if (snprintf(path, sizeof(path), "%s/.config/labwc/rc.xml", home) >= (int)sizeof(path)) {
        return 0;
    }

    FILE *config = fopen(path, "r");
    if (config == NULL) {
        config = fopen("/etc/xdg/labwc/rc.xml", "r");
    }

    if (config == NULL) {
        return 0;
    }

    char line[256];
    int theme = 0;
    while (fgets(line, sizeof(line), config) != NULL) {
        if (strstr(line, "<name>Arc-Clone</name>") != NULL) {
            theme = 1;
            break;
        }

        if (strstr(line, "<name>Penumbra</name>") != NULL) {
            theme = 2;
            break;
        }

        if (strstr(line, "<name>SynapseOS-Dark</name>") != NULL) {
            theme = 3;
            break;
        }
    }

    fclose(config);
    return theme;
}

struct panel_color {
    double red;
    double green;
    double blue;
};

struct panel_palette {
    struct panel_color background;
    struct panel_color border;
    struct panel_color button;
    struct panel_color active_button;
    struct panel_color accent;
    struct panel_color text;
};

static const struct panel_palette palettes[] = {
    {{0.19, 0.20, 0.23}, {0.37, 0.39, 0.43}, {0.26, 0.28, 0.31},
     {0.39, 0.25, 0.24}, {0.84, 0.36, 0.23}, {0.96, 0.96, 0.96}},
    {{0.91, 0.92, 0.94}, {0.67, 0.72, 0.77}, {0.85, 0.88, 0.92},
     {0.69, 0.80, 0.91}, {0.32, 0.56, 0.82}, {0.12, 0.16, 0.20}},
    {{0.14, 0.22, 0.27}, {0.27, 0.40, 0.46}, {0.20, 0.30, 0.36},
     {0.32, 0.46, 0.53}, {0.46, 0.67, 0.74}, {0.93, 0.96, 0.97}},
    {{0.10, 0.12, 0.15}, {0.25, 0.32, 0.38}, {0.14, 0.16, 0.20},
     {0.24, 0.30, 0.37}, {0.32, 0.56, 0.82}, {1.00, 1.00, 1.00}},
};

// Передать Cairo цвет выбранной темы
static void set_panel_color(cairo_t *context, struct panel_color color) {
    cairo_set_source_rgb(context, color.red, color.green, color.blue);
}

// Построить скруглённую область кнопки
static void rounded_rectangle(cairo_t *context, double x, double y, double width,
    double height, double radius) {
    const double quarter_turn = 1.5707963267948966;

    cairo_new_sub_path(context);
    cairo_arc(context, x + width - radius, y + radius, radius,
        -quarter_turn, 0.0);
    cairo_arc(context, x + width - radius, y + height - radius, radius,
        0.0, quarter_turn);
    cairo_arc(context, x + radius, y + height - radius, radius,
        quarter_turn, 2.0 * quarter_turn);
    cairo_arc(context, x + radius, y + radius, radius,
        2.0 * quarter_turn, 3.0 * quarter_turn);
    cairo_close_path(context);
}

// Освободить буфер после завершения чтения композитором
static void buffer_released(void *data, struct wl_buffer *handle) {
    struct panel_buffer *buffer = data;

    wl_buffer_destroy(handle);
    munmap(buffer->pixels, buffer->size);
    free(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_released,
};

// Создать буфер для слоя Wayland
static struct panel_buffer *create_buffer(int width, int height) {
    if (width <= 0 || height <= 0 || width > INT32_MAX / 4 ||
        height > INT32_MAX / (width * 4)) {
        return NULL;
    }

    size_t size = (size_t)width * (size_t)height * 4U;
    char path[] = "/tmp/synapse-panel-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return NULL;
    }

    unlink(path);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NULL;
    }

    struct panel_buffer *buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        close(fd);
        return NULL;
    }

    buffer->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->pixels == MAP_FAILED) {
        close(fd);
        free(buffer);
        return NULL;
    }

    buffer->size = size;
    struct wl_shm_pool *pool = wl_shm_create_pool(panel.shm, fd, (int)size);
    close(fd);
    if (pool == NULL) {
        munmap(buffer->pixels, size);
        free(buffer);
        return NULL;
    }

    buffer->handle = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (buffer->handle == NULL) {
        munmap(buffer->pixels, size);
        free(buffer);
        return NULL;
    }

    return buffer;
}

// Нарисовать обои в слое под окнами
static void background_draw(void) {
    if (panel.background_surface == NULL || panel.background_width < 1 ||
        panel.background_height < 1) {
        return;
    }

    struct panel_buffer *buffer = create_buffer(panel.background_width, panel.background_height);
    if (buffer == NULL) {
        return;
    }

    cairo_surface_t *image = cairo_image_surface_create_for_data(buffer->pixels,
        CAIRO_FORMAT_ARGB32, panel.background_width, panel.background_height,
        panel.background_width * 4);
    cairo_t *context = cairo_create(image);
    const char *path = panel.theme == 1 ?
        "/usr/share/backgrounds/synapse/Desktop-light.png" :
        "/usr/share/backgrounds/synapse/Desktop-dark.png";
    cairo_surface_t *wallpaper = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(wallpaper) == CAIRO_STATUS_SUCCESS) {
        double source_width = cairo_image_surface_get_width(wallpaper);
        double source_height = cairo_image_surface_get_height(wallpaper);
        if (source_width > 0 && source_height > 0) {
            double scale_x = panel.background_width / source_width;
            double scale_y = panel.background_height / source_height;
            double scale = scale_x > scale_y ? scale_x : scale_y;
            cairo_translate(context, (panel.background_width - source_width * scale) / 2.0,
                (panel.background_height - source_height * scale) / 2.0);
            cairo_scale(context, scale, scale);
            cairo_set_source_surface(context, wallpaper, 0, 0);
            cairo_paint(context);
        }
    }

    cairo_surface_destroy(wallpaper);
    cairo_destroy(context);
    cairo_surface_destroy(image);
    wl_buffer_add_listener(buffer->handle, &buffer_listener, buffer);
    wl_surface_attach(panel.background_surface, buffer->handle, 0, 0);
    wl_surface_damage_buffer(panel.background_surface, 0, 0,
        panel.background_width, panel.background_height);
    wl_surface_commit(panel.background_surface);
}

// Нарисовать одну кнопку на панели
static void draw_button(cairo_t *context, int x, int width, const char *label, bool active) {
    const struct panel_palette *colors = &palettes[panel.theme];
    set_panel_color(context, active ? colors->active_button : colors->button);
    rounded_rectangle(context, x + 3, 5, width - 6, panel.height - 10, 6.0);
    cairo_fill(context);

    if (active) {
        set_panel_color(context, colors->accent);
        rounded_rectangle(context, x + 12, panel.height - 5, width - 24, 3, 1.5);
        cairo_fill(context);
    }

    cairo_save(context);
    cairo_rectangle(context, x + 8, 0, width - 16, panel.height);
    cairo_clip(context);
    set_panel_color(context, colors->text);
    cairo_move_to(context, x + 13, panel.height / 2 + 5);
    cairo_show_text(context, label);
    cairo_restore(context);
}

// Освободить загруженный значок перед обновлением или выходом
static void clear_menu_icon(void) {
    if (panel.menu_icon != NULL) {
        cairo_surface_destroy(panel.menu_icon);
        panel.menu_icon = NULL;
    }

    panel.menu_icon_valid = false;
}

// Обновить значок после изменения файла и нарисовать его на кнопке меню
static void draw_menu_icon(cairo_t *context) {
    const char *path = "/usr/share/weston/synapse-logo.png";
    struct stat current;
    if (stat(path, &current) != 0 || !S_ISREG(current.st_mode)) {
        clear_menu_icon();
        return;
    }

    bool changed = !panel.menu_icon_valid ||
        current.st_dev != panel.menu_icon_stat.st_dev ||
        current.st_ino != panel.menu_icon_stat.st_ino ||
        current.st_size != panel.menu_icon_stat.st_size ||
        current.st_mtim.tv_sec != panel.menu_icon_stat.st_mtim.tv_sec ||
        current.st_mtim.tv_nsec != panel.menu_icon_stat.st_mtim.tv_nsec ||
        current.st_ctim.tv_sec != panel.menu_icon_stat.st_ctim.tv_sec ||
        current.st_ctim.tv_nsec != panel.menu_icon_stat.st_ctim.tv_nsec;
    if (changed) {
        cairo_surface_t *updated = cairo_image_surface_create_from_png(path);
        clear_menu_icon();
        if (updated == NULL || cairo_surface_status(updated) != CAIRO_STATUS_SUCCESS) {
            if (updated != NULL) {
                cairo_surface_destroy(updated);
            }

            return;
        }

        int width = cairo_image_surface_get_width(updated);
        int height = cairo_image_surface_get_height(updated);
        if (width <= 0 || height <= 0) {
            cairo_surface_destroy(updated);
            return;
        }

        cairo_surface_t *scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
            PANEL_MENU_ICON_SIZE, PANEL_MENU_ICON_SIZE);
        if (scaled == NULL || cairo_surface_status(scaled) != CAIRO_STATUS_SUCCESS) {
            if (scaled != NULL) {
                cairo_surface_destroy(scaled);
            }

            cairo_surface_destroy(updated);
            return;
        }

        cairo_t *scaler = cairo_create(scaled);
        if (scaler == NULL || cairo_status(scaler) != CAIRO_STATUS_SUCCESS) {
            if (scaler != NULL) {
                cairo_destroy(scaler);
            }

            cairo_surface_destroy(scaled);
            cairo_surface_destroy(updated);
            return;
        }

        double scale = (double)PANEL_MENU_ICON_SIZE / (double)(width > height ? width : height);
        cairo_translate(scaler, (PANEL_MENU_ICON_SIZE - width * scale) / 2.0,
            (PANEL_MENU_ICON_SIZE - height * scale) / 2.0);
        cairo_scale(scaler, scale, scale);
        cairo_set_source_surface(scaler, updated, 0, 0);
        cairo_paint(scaler);
        cairo_destroy(scaler);
        cairo_surface_destroy(updated);

        panel.menu_icon = scaled;
        panel.menu_icon_stat = current;
        panel.menu_icon_valid = true;
    }

    cairo_save(context);
    cairo_set_source_surface(context, panel.menu_icon, 12,
        (panel.height - PANEL_MENU_ICON_SIZE) / 2.0);
    cairo_paint(context);
    cairo_restore(context);
}

// Подготовить разделяемый буфер и обновить видимую панель
static void panel_draw(void) {
    if (panel.surface == NULL || panel.width < 320 || panel.height < 20) {
        return;
    }

    struct panel_buffer *buffer = create_buffer(panel.width, panel.height);
    if (buffer == NULL) {
        return;
    }

    cairo_surface_t *image = cairo_image_surface_create_for_data(buffer->pixels,
        CAIRO_FORMAT_ARGB32, panel.width, panel.height, panel.width * 4);
    cairo_t *context = cairo_create(image);
    const struct panel_palette *colors = &palettes[panel.theme];
    set_panel_color(context, colors->background);
    cairo_paint(context);
    set_panel_color(context, colors->border);
    cairo_rectangle(context, 0, 0, panel.width, 1);
    cairo_fill(context);
    cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 15);

    draw_button(context, 0, PANEL_MENU_WIDTH, "     Меню", false);
    draw_menu_icon(context);
    draw_button(context, PANEL_MENU_WIDTH, PANEL_FILES_WIDTH, "Файлы", false);
    draw_button(context, PANEL_MENU_WIDTH + PANEL_FILES_WIDTH, PANEL_TERMINAL_WIDTH,
        "Терминал", false);

    int x = PANEL_MENU_WIDTH + PANEL_FILES_WIDTH + PANEL_TERMINAL_WIDTH;
    for (struct panel_window *window = panel.windows; window != NULL; window = window->next) {
        if (x + PANEL_TASK_WIDTH > panel.width - 195) {
            break;
        }

        draw_button(context, x, PANEL_TASK_WIDTH, window->title != NULL ? window->title : "Окно",
            window->active);
        x += PANEL_TASK_WIDTH;
    }

    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) != NULL) {
        char clock_text[32];
        if (strftime(clock_text, sizeof(clock_text), "%d.%m.%Y %H:%M", &local) != 0) {
            set_panel_color(context, colors->text);
            cairo_move_to(context, panel.width - 180, panel.height / 2 + 5);
            cairo_show_text(context, clock_text);
        }
    }

    cairo_destroy(context);
    cairo_surface_destroy(image);
    wl_buffer_add_listener(buffer->handle, &buffer_listener, buffer);
    wl_surface_attach(panel.surface, buffer->handle, 0, 0);
    wl_surface_damage_buffer(panel.surface, 0, 0, panel.width, panel.height);
    wl_surface_commit(panel.surface);
}

// Запустить приложение из кнопки панели
static void panel_launch(const char *path, const char *argument) {
    pid_t child = fork();
    if (child == 0) {
        if (argument == NULL) {
            execl(path, path, (char *)NULL);
        } else {
            execl(path, path, argument, (char *)NULL);
        }

        _exit(127);
    }
}

// Обработать кнопку мыши на панели
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state) {
    (void)data;
    (void)pointer;
    (void)serial;
    (void)time;

    if (button != 0x110 || state != WL_POINTER_BUTTON_STATE_PRESSED) {
        return;
    }

    if (panel.pointer_x < PANEL_MENU_WIDTH) {
        panel_launch("/bin/synapse-menuctl", NULL);
    } else if (panel.pointer_x < PANEL_MENU_WIDTH + PANEL_FILES_WIDTH) {
        panel_launch("/bin/synapse-files", NULL);
    } else if (panel.pointer_x < PANEL_MENU_WIDTH + PANEL_FILES_WIDTH + PANEL_TERMINAL_WIDTH) {
        panel_launch("/bin/weston-terminal", NULL);
    } else {
        int index = (panel.pointer_x - PANEL_MENU_WIDTH - PANEL_FILES_WIDTH - PANEL_TERMINAL_WIDTH) /
            PANEL_TASK_WIDTH;
        for (struct panel_window *window = panel.windows; window != NULL; window = window->next) {
            if (index-- == 0) {
                if (window->active) {
                    zwlr_foreign_toplevel_handle_v1_set_minimized(window->handle);
                } else {
                    if (window->minimized) {
                        zwlr_foreign_toplevel_handle_v1_unset_minimized(window->handle);
                    }

                    zwlr_foreign_toplevel_handle_v1_activate(window->handle, panel.seat);
                }

                break;
            }
        }
    }
}

// Запомнить положение мыши для выбора кнопки
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
    wl_fixed_t x, wl_fixed_t y) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)y;
    panel.pointer_x = wl_fixed_to_int(x);
}

// Принять вход указателя в область панели
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;
    (void)y;
    panel.pointer_x = wl_fixed_to_int(x);
}

// Принять выход указателя из области панели
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface) {
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;
}

// Игнорировать прокрутку над кнопками панели
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
    uint32_t axis, wl_fixed_t value) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

// Получить указатель из устройства ввода
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    (void)data;

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && panel.pointer == NULL) {
        panel.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(panel.pointer, &pointer_listener, NULL);
    }
}

// Принять имя устройства ввода
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

// Обновить название окна после события композитора
static void window_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    const char *title) {
    (void)handle;
    struct panel_window *window = data;
    char *copy = strdup(title);
    if (copy != NULL) {
        free(window->title);
        window->title = copy;
    }
}

// Отметить активное окно
static void window_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_array *states) {
    (void)handle;
    struct panel_window *window = data;
    window->active = false;
    window->minimized = false;

    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
            window->active = true;
        }

        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) {
            window->minimized = true;
        }
    }
}

// Принять идентификатор приложения без изменения заголовка
static void window_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    const char *app_id) {
    (void)data;
    (void)handle;
    (void)app_id;
}

// Принять перемещение окна между экранами
static void window_output(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_output *output) {
    (void)data;
    (void)handle;
    (void)output;
}

// Принять изменение родительского окна
static void window_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data;
    (void)handle;
    (void)parent;
}

// Перерисовать панель после завершения набора изменений окна
static void window_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)data;
    (void)handle;
    panel_draw();
}

// Убрать закрытое окно из панели
static void window_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct panel_window *window = data;
    struct panel_window **entry = &panel.windows;

    while (*entry != NULL && *entry != window) {
        entry = &(*entry)->next;
    }

    if (*entry == window) {
        *entry = window->next;
    }

    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    free(window->title);
    free(window);
    panel_draw();
}

static const struct zwlr_foreign_toplevel_handle_v1_listener window_listener = {
    .title = window_title,
    .app_id = window_app_id,
    .output_enter = window_output,
    .output_leave = window_output,
    .state = window_state,
    .done = window_done,
    .closed = window_closed,
    .parent = window_parent,
};

// Добавить новое окно в список задач
static void manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
    struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)data;
    (void)manager;
    struct panel_window *window = calloc(1, sizeof(*window));
    if (window == NULL) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }

    window->handle = handle;
    window->next = panel.windows;
    panel.windows = window;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &window_listener, window);
}

// Завершить при закрытии списка окон композитором
static void manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {
    (void)data;
    (void)manager;
    exit(0);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished,
};

// Принять размер панели от композитора
static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
    uint32_t serial, uint32_t width, uint32_t height) {
    (void)data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    panel.width = (int)width;
    panel.height = (int)height;
    panel_draw();
}

// Завершить работу, когда композитор закрыл панель
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
    (void)data;
    (void)layer;
    exit(0);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

// Принять размеры рабочего стола для фонового слоя
static void background_configure(void *data, struct zwlr_layer_surface_v1 *layer,
    uint32_t serial, uint32_t width, uint32_t height) {
    (void)data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    panel.background_width = (int)width;
    panel.background_height = (int)height;
    background_draw();
}

// Оставить фон swaybg при закрытии фонового слоя
static void background_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
    (void)data;
    (void)layer;
    panel.background_surface = NULL;
}

static const struct zwlr_layer_surface_v1_listener background_listener = {
    .configure = background_configure,
    .closed = background_closed,
};

// Подключить необходимые интерфейсы Wayland
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version) {
    (void)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        panel.compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
            version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        panel.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        panel.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(panel.seat, &seat_listener, NULL);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        panel.shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        panel.manager = wl_registry_bind(registry, name,
            &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(panel.manager, &manager_listener, NULL);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
};

// Создать панель и обрабатывать окна до выхода из сеанса
int main(void) {
    panel.theme = read_theme();
    panel.display = wl_display_connect(NULL);
    if (panel.display == NULL) {
        fprintf(stderr, "synapse-panel: Wayland unavailable\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(panel.display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(panel.display) < 0 || panel.compositor == NULL ||
        panel.shm == NULL || panel.shell == NULL || panel.seat == NULL ||
        panel.manager == NULL) {
        fprintf(stderr, "synapse-panel: required protocol unavailable\n");
        return 1;
    }

    panel.background_surface = wl_compositor_create_surface(panel.compositor);
    if (panel.background_surface != NULL) {
        struct wl_region *input = wl_compositor_create_region(panel.compositor);
        if (input != NULL) {
            wl_surface_set_input_region(panel.background_surface, input);
            wl_region_destroy(input);
        }

        panel.background_layer = zwlr_layer_shell_v1_get_layer_surface(panel.shell,
            panel.background_surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
            "synapse-background");
        if (panel.background_layer != NULL) {
            zwlr_layer_surface_v1_set_anchor(panel.background_layer,
                ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            zwlr_layer_surface_v1_set_size(panel.background_layer, 0, 0);
            zwlr_layer_surface_v1_set_exclusive_zone(panel.background_layer, -1);
            zwlr_layer_surface_v1_add_listener(panel.background_layer,
                &background_listener, NULL);
            wl_surface_commit(panel.background_surface);
        }
    }

    panel.surface = wl_compositor_create_surface(panel.compositor);
    panel.layer = zwlr_layer_shell_v1_get_layer_surface(panel.shell, panel.surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "synapse-panel");
    zwlr_layer_surface_v1_set_anchor(panel.layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(panel.layer, 0, 44);
    zwlr_layer_surface_v1_set_exclusive_zone(panel.layer, 44);
    zwlr_layer_surface_v1_add_listener(panel.layer, &layer_listener, NULL);
    wl_surface_commit(panel.surface);
    if (wl_display_roundtrip(panel.display) < 0) {
        return 1;
    }

    while (wl_display_dispatch_pending(panel.display) >= 0) {
        if (wl_display_flush(panel.display) < 0 && errno != EAGAIN) {
            break;
        }

        struct pollfd wait = {.fd = wl_display_get_fd(panel.display), .events = POLLIN};
        int ready = poll(&wait, 1, 1000);
        if (ready > 0 && (wait.revents & POLLIN) != 0) {
            if (wl_display_dispatch(panel.display) < 0) {
                break;
            }
        } else if (ready < 0 && errno != EINTR) {
            break;
        }

        time_t now = time(NULL);
        struct tm local;
        if (localtime_r(&now, &local) != NULL && local.tm_min != panel.last_minute) {
            panel.last_minute = local.tm_min;
            panel_draw();
        }

        int theme = read_theme();
        if (theme != panel.theme) {
            panel.theme = theme;
            clear_menu_icon();
            panel_draw();
            background_draw();
        }
    }

    clear_menu_icon();
    wl_display_disconnect(panel.display);
    return 0;
}
