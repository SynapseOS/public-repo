// panel.c: Панель окон рабочего стола Labwc
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: AArch64 и x86_64, ABI: musl и Wayland
// Описание: Показывать меню, работающие окна и часы на нижней панели
// Подсистема: графическая оболочка
// Первое изменение: 28.10.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <cairo.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/file.h>
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
extern char **environ;

struct panel_window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct panel_window *next;
    char *title;
    char *app_id;
    cairo_surface_t *icon;
    bool active;
    bool minimized, hidden_by_desktop, was_active;
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
    bool dirty;
    bool background_committed;
    struct wl_callback *frame;
    int background_width;
    int background_height;
    int pointer_x, pointer_y;
    struct wl_surface *pointer_surface;
    char desktop_dir[PATH_MAX];
    struct dirent **desktop_files;
    int desktop_count, selected, last_index;
    uint32_t last_click;
    struct timespec desktop_mtime;
    bool desktop_scanned;
    bool keyboard_available;
    pid_t keyboard_pid;
    int last_minute;
    int theme;
    int panel_height;
    time_t wallpaper_mtime;
    cairo_surface_t *menu_icon;
    struct stat menu_icon_stat;
    bool menu_icon_valid;
} panel;

enum {
    PANEL_MENU_WIDTH = 52,
    PANEL_PIN_WIDTH = 48,
    PANEL_PIN_COUNT = 4,
    PANEL_LAUNCHERS_WIDTH = PANEL_MENU_WIDTH + PANEL_PIN_WIDTH * PANEL_PIN_COUNT,
    PANEL_TASK_WIDTH = 48,
    PANEL_MENU_ICON_SIZE = 24,
    PANEL_DESKTOP_WIDTH = 36,
    PANEL_CLOCK_WIDTH = 118,
    PANEL_TRAY_WIDTH = 104,
    PANEL_KEYBOARD_WIDTH = 40,
    PANEL_RECORDING_WIDTH = 58,
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
        if (strstr(line, "<name>synapse-classic</name>") != NULL) {
            theme = 4;
            break;
        }

        if (strstr(line, "<name>Arc-Clone</name>") != NULL ||
            strstr(line, "<name>Synapse-Light</name>") != NULL) {
            theme = 1;
            break;
        }

        if (strstr(line, "<name>Synapse-Midnight</name>") != NULL ||
            strstr(line, "<name>Synapse-Graphite</name>") != NULL ||
            strstr(line, "<name>Synapse-Ocean</name>") != NULL ||
            strstr(line, "<name>Synapse-Dracula</name>") != NULL) {
            theme = 3;
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

static struct panel_palette palettes[] = {
    {{0.19, 0.20, 0.23}, {0.37, 0.39, 0.43}, {0.26, 0.28, 0.31},
     {0.39, 0.25, 0.24}, {0.84, 0.36, 0.23}, {0.96, 0.96, 0.96}},
    {{0.91, 0.92, 0.94}, {0.67, 0.72, 0.77}, {0.85, 0.88, 0.92},
     {0.69, 0.80, 0.91}, {0.32, 0.56, 0.82}, {0.12, 0.16, 0.20}},
    {{0.14, 0.22, 0.27}, {0.27, 0.40, 0.46}, {0.20, 0.30, 0.36},
     {0.32, 0.46, 0.53}, {0.46, 0.67, 0.74}, {0.93, 0.96, 0.97}},
    {{0.10, 0.12, 0.15}, {0.25, 0.32, 0.38}, {0.14, 0.16, 0.20},
     {0.24, 0.30, 0.37}, {0.32, 0.56, 0.82}, {1.00, 1.00, 1.00}},
    {{0.094, 0.361, 0.569}, {0.094, 0.361, 0.569}, {0.094, 0.361, 0.569},
     {0.180, 0.439, 0.627}, {0.561, 0.765, 0.910}, {1.0, 1.0, 1.0}},
};

// Прочитать переносимые параметры оформления нижней панели
static void read_appearance(void) {
    panel.panel_height = 44;
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == 0) {
        return;
    }

    char path[512];
    if (snprintf(path, sizeof(path), "%s/.config/SynapseOS/Desktop.conf", home) >= (int)sizeof(path)) {
        return;
    }
    FILE *input = fopen(path, "r");
    if (input == NULL) {
        return;
    }

    bool appearance = false;
    char line[256];
    while (fgets(line, sizeof(line), input) != NULL) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') {
            appearance = strcmp(line, "[appearance]") == 0;
            continue;
        }
        if (!appearance) {
            continue;
        }
        char *separator = strchr(line, '=');
        if (separator == NULL) {
            continue;
        }
        *separator++ = 0;
        if (!strcmp(line, "panel-height")) {
            int value = atoi(separator);
            panel.panel_height = value >= 32 && value <= 96 ? value : panel.panel_height;
            continue;
        }

        struct panel_color *target = NULL;
        if (!strcmp(line, "background")) target = &palettes[panel.theme].background;
        if (!strcmp(line, "base")) target = &palettes[panel.theme].button;
        if (!strcmp(line, "surface")) target = &palettes[panel.theme].button;
        if (!strcmp(line, "foreground")) target = &palettes[panel.theme].text;
        if (!strcmp(line, "accent")) target = &palettes[panel.theme].accent;
        unsigned red, green, blue;
        if (target != NULL && sscanf(separator, "#%2x%2x%2x", &red, &green, &blue) == 3 &&
            strlen(separator) == 7) {
            target->red = red / 255.0;
            target->green = green / 255.0;
            target->blue = blue / 255.0;
        }
    }
    fclose(input);
}

// Передать Cairo цвет выбранной темы
static void set_panel_color(cairo_t *context, struct panel_color color) {
    cairo_set_source_rgb(context, color.red, color.green, color.blue);
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
    char shared_path[] = "/dev/shm/synapse-panel-XXXXXX";
    char tmp_path[] = "/tmp/synapse-panel-XXXXXX";
    char *path = shared_path;
    int fd = mkstemp(path);
    if (fd < 0 && errno == ENOENT) {
        path = tmp_path;
        fd = mkstemp(path);
    }
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

enum { IconWidth = 114, IconHeight = 88, IconMargin = 18, Shortcuts = 3 };
enum IconKind { Folder, Applications, Preferences, Document };

static int visible_name(const struct dirent *entry) {
    return entry->d_name[0] != '.';
}

// Перечитать настоящий каталог пользователя при добавлении и переименовании файлов
static bool scan_desktop(void) {
    struct stat st;
    if (!panel.desktop_dir[0] || stat(panel.desktop_dir, &st) < 0) {
        return false;
    }
    if (panel.desktop_scanned && st.st_mtim.tv_sec == panel.desktop_mtime.tv_sec &&
        st.st_mtim.tv_nsec == panel.desktop_mtime.tv_nsec) {
        return false;
    }
    struct dirent **entries;
    int count = scandir(panel.desktop_dir, &entries, visible_name, alphasort);
    if (count < 0) {
        return false;
    }
    for (int i = 0; i < panel.desktop_count; i++) free(panel.desktop_files[i]);
    free(panel.desktop_files);
    panel.desktop_files = entries;
    panel.desktop_count = count;
    panel.desktop_mtime = st.st_mtim;
    panel.desktop_scanned = true;
    panel.last_index = panel.selected = -1;
    return true;
}

static void desktop_cell(int index, int *x, int *y) {
    int rows = (panel.background_height - 72) / IconHeight;
    if (rows < 1) rows = 1;
    *x = IconMargin + index / rows * IconWidth;
    *y = IconMargin + index % rows * IconHeight;
}

static void desktop_glyph(cairo_t *cr, enum IconKind kind) {
    if (kind == Folder) {
        cairo_set_source_rgb(cr, .20, .49, .74);
        cairo_rectangle(cr, -22, 5, 21, 16);
        cairo_fill(cr);
        cairo_rectangle(cr, -22, 11, 44, 28);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, .43, .73, .96);
        cairo_rectangle(cr, -22, 16, 44, 23);
        cairo_fill(cr);
    } else if (kind == Applications) {
        for (int row = 0; row < 2; row++) {
            for (int col = 0; col < 2; col++) {
                cairo_set_source_rgb(cr, .42 + .10 * col, .68 + .10 * row, .90);
                cairo_rectangle(cr, -19 + 21 * col, 4 + 20 * row, 17, 16);
                cairo_fill(cr);
            }
        }
    } else if (kind == Preferences) {
        cairo_set_source_rgb(cr, .75, .84, .91);
        cairo_set_line_width(cr, 3);
        for (int row = 0; row < 3; row++) {
            double y = 9 + 12 * row;
            cairo_move_to(cr, -17, y); cairo_line_to(cr, 17, y); cairo_stroke(cr);
            cairo_arc(cr, row == 1 ? -7 : 7, y, 4.5, 0, 6.2831853); cairo_fill(cr);
        }
    } else {
        cairo_set_source_rgb(cr, .91, .93, .97);
        cairo_rectangle(cr, -15, 3, 30, 38);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, .46, .53, .62);
        for (int line = 0; line < 3; line++) {
            cairo_rectangle(cr, -9, 13 + line * 6, line == 2 ? 11 : 17, 2); cairo_fill(cr);
        }
    }
}

static const char *desktop_name(int i, bool russian) {
    const char *ru[] = {"Мои файлы", "Приложения", "Настройки"};
    const char *en[] = {"Home", "Applications", "Settings"};
    return i < Shortcuts ? (russian ? ru[i] : en[i]) : panel.desktop_files[i - Shortcuts]->d_name;
}

static void desktop_icons(cairo_t *cr) {
    const char *lang = getenv("LANG");
    bool russian = lang && !strncmp(lang, "ru", 2);
    struct wl_region *input = wl_compositor_create_region(panel.compositor);
    for (int i = 0; i < Shortcuts + panel.desktop_count; i++) {
        int x, y;
        desktop_cell(i, &x, &y);
        if (x + IconWidth > panel.background_width) break;
        if (input) wl_region_add(input, x, y, IconWidth - 4, IconHeight - 4);
        cairo_save(cr);
        cairo_translate(cr, x, y);
        if (panel.selected == i) {
            cairo_set_source_rgba(cr, .21, .46, .80, .30);
            cairo_rectangle(cr, 0, 0, IconWidth - 4, IconHeight - 4);
            cairo_fill(cr);
        }
        cairo_translate(cr, (IconWidth - 4) / 2.0, 4);
        enum IconKind kind = i < Shortcuts ? (enum IconKind)i :
            panel.desktop_files[i - Shortcuts]->d_type == DT_DIR ? Folder : Document;
        desktop_glyph(cr, kind);
        cairo_rectangle(cr, -(IconWidth - 6)/2.0, 43, IconWidth - 6, 30); cairo_clip(cr);
        cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        const char *label = desktop_name(i, russian);
        // Ошибочное имя файла не должно перевести общий контекст Cairo в состояние ошибки
        cairo_t *text = cairo_create(cairo_get_target(cr));
        cairo_matrix_t matrix;
        cairo_get_matrix(cr, &matrix);
        cairo_set_matrix(text, &matrix);
        cairo_rectangle(text, -(IconWidth - 6)/2.0, 43, IconWidth - 6, 30); cairo_clip(text);
        cairo_set_scaled_font(text, cairo_get_scaled_font(cr));
        cairo_text_extents_t ext;
        cairo_text_extents(text, label, &ext);
        double left = ext.width < IconWidth - 8 ? -ext.width / 2.0 - ext.x_bearing : -IconWidth / 2.0 + 4;
        cairo_move_to(text, left, 63);
        cairo_text_path(text, label);
        if (panel.theme == 4) {
            cairo_set_source_rgb(text, .19, .21, .23);
        } else {
            cairo_set_source_rgb(text, 1, 1, 1);
        }
        cairo_fill(text);
        cairo_destroy(text);
        cairo_restore(cr);
    }
    if (input) {
        wl_surface_set_input_region(panel.background_surface, input);
        wl_region_destroy(input);
    }
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
    const char *path = panel.theme == 1 || panel.theme == 4 ?
        "/usr/share/backgrounds/synapse/Desktop-light.png" :
        "/usr/share/backgrounds/synapse/Desktop-dark.png";
    const char *home = getenv("HOME");
    char config_path[512], custom[1024];
    if (home != NULL && snprintf(config_path, sizeof(config_path),
        "%s/.config/synapse/wallpaper", home) < (int)sizeof(config_path)) {
        FILE *config = fopen(config_path, "r");
        if (config != NULL && fgets(custom, sizeof(custom), config) != NULL) {
            custom[strcspn(custom, "\r\n")] = 0;
            if (custom[0] == '/' && access(custom, R_OK) == 0) {
                path = custom;
            }
        }
        if (config != NULL) {
            fclose(config);
        }
    }
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
    cairo_identity_matrix(context);
    desktop_icons(context);
    cairo_destroy(context);
    cairo_surface_destroy(image);
    wl_buffer_add_listener(buffer->handle, &buffer_listener, buffer);
    wl_surface_attach(panel.background_surface, buffer->handle, 0, 0);
    wl_surface_damage_buffer(panel.background_surface, 0, 0,
        panel.background_width, panel.background_height);
    wl_surface_commit(panel.background_surface);
    panel.background_committed = true;
}

static char recording;

// Определить закреплённую кнопку для открытого окна
static int pinned_index(const struct panel_window *window) {
    if (window->title != NULL && strstr(window->title, "Настройки SynapseOS") != NULL) {
        return 3;
    }

    if (window->app_id == NULL) {
        return -1;
    }
    if (strstr(window->app_id, "synapse-files") != NULL) {
        return 0;
    }
    if (strstr(window->app_id, "synapse-store") != NULL) {
        return 1;
    }
    if (strstr(window->app_id, "wayland-terminal") != NULL ||
        strstr(window->app_id, "weston-terminal") != NULL) {
        return 2;
    }

    return -1;
}

// Найти открытое окно для закреплённой кнопки
static struct panel_window *pinned_window(int index) {
    struct panel_window *first = NULL;
    for (struct panel_window *window = panel.windows; window != NULL; window = window->next) {
        if (pinned_index(window) != index) {
            continue;
        }
        if (window->active) {
            return window;
        }
        if (first == NULL) {
            first = window;
        }
    }

    return first;
}

// Свернуть активное окно или вывести его на передний план
static void toggle_window(struct panel_window *window) {
    if (window->active) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(window->handle);
    } else {
        if (window->minimized) {
            zwlr_foreign_toplevel_handle_v1_unset_minimized(window->handle);
        }

        zwlr_foreign_toplevel_handle_v1_activate(window->handle, panel.seat);
    }
}

// Загрузить значок PNG с проверкой размера
static cairo_surface_t *load_png_icon(const char *path) {
    struct stat details;
    if (path == NULL || stat(path, &details) != 0 || !S_ISREG(details.st_mode) ||
        details.st_size < 1 || details.st_size > 4 * 1024 * 1024) {
        return NULL;
    }

    cairo_surface_t *icon = cairo_image_surface_create_from_png(path);
    if (icon == NULL) {
        return NULL;
    }
    if (cairo_surface_status(icon) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_width(icon) < 1 || cairo_image_surface_get_width(icon) > 1024 ||
        cairo_image_surface_get_height(icon) < 1 || cairo_image_surface_get_height(icon) > 1024) {
        cairo_surface_destroy(icon);
        return NULL;
    }

    return icon;
}

// Найти значок по идентификатору окна в установленных приложениях
static cairo_surface_t *load_window_icon(const char *app_id) {
    if (app_id == NULL || app_id[0] == 0) {
        return NULL;
    }
    char name[128];
    size_t length = strnlen(app_id, sizeof(name));
    if (length == 0 || length >= sizeof(name)) {
        return NULL;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char part = (unsigned char)app_id[index];
        if (!((part >= 'a' && part <= 'z') || (part >= 'A' && part <= 'Z') ||
            (part >= '0' && part <= '9') || part == '.' || part == '_' || part == '-')) {
            return NULL;
        }
    }

    if (strcasestr(app_id, "mindustry") != NULL) {
        return load_png_icon("/usr/share/weston/mindustry-icon.png");
    }
    if (strcasestr(app_id, "synapse-camera") != NULL) {
        return load_png_icon("/usr/share/icons/hicolor/128x128/apps/synapse-camera.png");
    }
    if (strcasestr(app_id, "synapse-dictaphone") != NULL) {
        return load_png_icon("/usr/share/icons/hicolor/128x128/apps/synapse-dictaphone.png");
    }
    if (strcasestr(app_id, "recorder") != NULL) {
        return load_png_icon("/usr/share/icons/hicolor/128x128/apps/synapse-recorder.png");
    }

    memcpy(name, app_id, length + 1);
    if (length > 8 && strcmp(name + length - 8, ".desktop") == 0) {
        name[length - 8] = 0;
    }

    static const char *directories[] = {
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/32x32/apps",
        "/usr/share/pixmaps",
        "/var/lib/flatpak/exports/share/icons/hicolor/128x128/apps",
        "/var/lib/flatpak/exports/share/icons/hicolor/64x64/apps"
    };
    char path[PATH_MAX];
    for (size_t index = 0; index < sizeof(directories) / sizeof(directories[0]); index++) {
        int written = snprintf(path, sizeof(path), "%s/%s.png", directories[index], name);
        if (written > 0 && written < (int)sizeof(path)) {
            cairo_surface_t *icon = load_png_icon(path);
            if (icon != NULL) {
                return icon;
            }
        }
    }

    const char *home = getenv("HOME");
    if (home != NULL && home[0] == '/') {
        int written = snprintf(path, sizeof(path),
            "%s/.local/share/flatpak/exports/share/icons/hicolor/128x128/apps/%s.png", home, name);
        if (written > 0 && written < (int)sizeof(path)) {
            return load_png_icon(path);
        }
    }
    return NULL;
}

// Нарисовать установленный значок по центру кнопки
static void draw_png_icon(cairo_t *context, int x, double center, cairo_surface_t *icon) {
    int width = cairo_image_surface_get_width(icon);
    int height = cairo_image_surface_get_height(icon);
    double scale = width > height ? 24.0 / width : 24.0 / height;

    cairo_save(context);
    cairo_translate(context, x - width * scale / 2.0, center - height * scale / 2.0);
    cairo_scale(context, scale, scale);
    cairo_set_source_surface(context, icon, 0, 0);
    cairo_paint(context);
    cairo_restore(context);
}

// Нарисовать значок программы или простой знак для приложения без PNG
static void draw_app_icon(cairo_t *context, int x, const char *app_id, bool active,
    cairo_surface_t *icon) {
    const struct panel_palette *colors = &palettes[panel.theme];
    double center = panel.height / 2.0;
    if (icon != NULL) {
        draw_png_icon(context, x, center, icon);
        return;
    }
    if (active) {
        cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    } else {
        set_panel_color(context, colors->text);
    }
    cairo_set_line_width(context, 1.7);
    cairo_set_line_cap(context, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(context, CAIRO_LINE_JOIN_ROUND);

    if (app_id != NULL && strcasestr(app_id, "terminal") != NULL) {
        cairo_rectangle(context, x - 10, center - 8, 20, 16);
        cairo_stroke(context);
        cairo_move_to(context, x - 6, center - 3);
        cairo_line_to(context, x - 2, center);
        cairo_line_to(context, x - 6, center + 3);
        cairo_move_to(context, x + 1, center + 4);
        cairo_line_to(context, x + 6, center + 4);
        cairo_stroke(context);
    } else if (app_id != NULL && (strcasestr(app_id, "settings") != NULL ||
        strcasestr(app_id, "preferences") != NULL)) {
        for (int row = -1; row <= 1; row++) {
            double line = center + row * 6;
            cairo_move_to(context, x - 9, line);
            cairo_line_to(context, x + 9, line);
            cairo_stroke(context);
            cairo_arc(context, x + (row == 0 ? 4 : -4), line, 2.3, 0.0, 6.2831853);
            cairo_fill(context);
        }
    } else if (app_id != NULL && strcasestr(app_id, "store") != NULL) {
        cairo_rectangle(context, x - 9, center - 5, 18, 15);
        cairo_stroke(context);
        cairo_arc(context, x, center - 5, 5, 3.1415926, 6.2831853);
        cairo_stroke(context);
        cairo_move_to(context, x - 4, center + 1);
        cairo_line_to(context, x + 4, center + 1);
        cairo_stroke(context);
    } else if (app_id != NULL && strcasestr(app_id, "monitor") != NULL) {
        cairo_move_to(context, x - 10, center + 6);
        cairo_line_to(context, x - 5, center);
        cairo_line_to(context, x, center + 3);
        cairo_line_to(context, x + 5, center - 7);
        cairo_line_to(context, x + 10, center - 2);
        cairo_stroke(context);
    } else if (app_id != NULL && strcasestr(app_id, "desktop") != NULL) {
        cairo_rectangle(context, x - 10, center - 8, 20, 14);
        cairo_stroke(context);
        cairo_move_to(context, x - 5, center + 9);
        cairo_line_to(context, x + 5, center + 9);
        cairo_move_to(context, x, center + 6);
        cairo_line_to(context, x, center + 9);
        cairo_stroke(context);
    } else if (app_id != NULL && (strcasestr(app_id, "file") != NULL ||
        strcasestr(app_id, "home") != NULL)) {
        cairo_rectangle(context, x - 10, center - 6, 20, 13);
        cairo_stroke(context);
        cairo_move_to(context, x - 9, center - 6);
        cairo_line_to(context, x - 4, center - 6);
        cairo_line_to(context, x - 1, center - 9);
        cairo_line_to(context, x + 5, center - 9);
        cairo_stroke(context);
    } else {
        cairo_rectangle(context, x - 10, center - 9, 20, 18);
        cairo_stroke(context);
        cairo_move_to(context, x - 10, center - 4);
        cairo_line_to(context, x + 10, center - 4);
        cairo_stroke(context);
        cairo_rectangle(context, x - 6, center - 1, 5, 5);
        cairo_fill(context);
        const char *name = app_id;
        if (name != NULL) {
            for (const char *part = app_id; *part != 0; part++) {
                if (*part == '.' || *part == '-' || *part == '_') {
                    name = part + 1;
                }
            }
        }
        char letter[2] = {'?', 0};
        if (name != NULL && ((name[0] >= 'a' && name[0] <= 'z') ||
            (name[0] >= 'A' && name[0] <= 'Z'))) {
            letter[0] = name[0] >= 'a' && name[0] <= 'z' ? name[0] - 'a' + 'A' : name[0];
        }
        cairo_save(context);
        cairo_select_font_face(context, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL,
            CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(context, 11);
        cairo_move_to(context, x + 2, center + 6);
        cairo_show_text(context, letter);
        cairo_restore(context);
    }
}

// Нарисовать одну кнопку на панели
static void draw_button(cairo_t *context, int x, int width, const char *label, bool active,
    const char *app_id, cairo_surface_t *icon) {
    const struct panel_palette *colors = &palettes[panel.theme];
    set_panel_color(context, active ? colors->active_button : colors->button);
    cairo_rectangle(context, x, 0, width, panel.height);
    cairo_fill(context);

    if (active) {
        set_panel_color(context, colors->accent);
        cairo_rectangle(context, x + 9, panel.height - 3, width - 18, 3);
        cairo_fill(context);
    }

    if (app_id != NULL) {
        draw_app_icon(context, label[0] == 0 ? x + width / 2 : x + 21, app_id, active, icon);
    }
    if (label[0] == 0) {
        return;
    }

    cairo_save(context);
    int text_offset = app_id != NULL && label[0] != 0 ? 38 : 12;
    cairo_rectangle(context, x + text_offset, 0, width - text_offset - 7, panel.height);
    cairo_clip(context);
    if (active) {
        cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    } else {
        set_panel_color(context, colors->text);
    }
    cairo_move_to(context, x + text_offset, panel.height / 2 + 5);
    cairo_show_text(context, label);
    cairo_restore(context);
}

// Нарисовать состояние сети, звука и раскладки
static void draw_system_tray(cairo_t *context, int x) {
    const struct panel_palette *colors = &palettes[panel.theme];
    double center = panel.height / 2.0;
    set_panel_color(context, colors->text);
    cairo_set_line_width(context, 1.7);
    cairo_set_line_cap(context, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(context);

    cairo_arc(context, x + 18, center + 3, 10, 3.70, 5.72);
    cairo_stroke(context);
    cairo_arc(context, x + 18, center + 3, 6, 3.70, 5.72);
    cairo_stroke(context);
    cairo_arc(context, x + 18, center + 7, 1.7, 0.0, 6.2831853);
    cairo_fill(context);

    cairo_move_to(context, x + 40, center - 3);
    cairo_line_to(context, x + 44, center - 3);
    cairo_line_to(context, x + 49, center - 8);
    cairo_line_to(context, x + 49, center + 8);
    cairo_line_to(context, x + 44, center + 3);
    cairo_line_to(context, x + 40, center + 3);
    cairo_close_path(context);
    cairo_stroke(context);
    cairo_arc(context, x + 49, center, 6, -0.85, 0.85);
    cairo_stroke(context);

    const char *layout = "en/ru";
    cairo_select_font_face(context, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 13);
    cairo_move_to(context, x + 60, center + 5);
    cairo_show_text(context, layout);
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
    set_panel_color(context, palettes[panel.theme].text);
    cairo_mask_surface(context, panel.menu_icon,
        (PANEL_MENU_WIDTH - PANEL_MENU_ICON_SIZE) / 2.0,
        (panel.height - PANEL_MENU_ICON_SIZE) / 2.0);
    cairo_restore(context);
}

static char active_recording(void) {
    char name[80], result=0;
    snprintf(name,sizeof(name),"/tmp/synapse-recorder-%lu",(unsigned long)getuid());
    int root=open(name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (root<0) return 0;
    struct stat st;
    if (fstat(root,&st) || st.st_uid!=getuid() || (st.st_mode&077)) { close(root); return 0; }
    DIR *dir=fdopendir(root);
    if (!dir) { close(root); return 0; }
    struct dirent *e;
    while ((e=readdir(dir))) {
        if (strncmp(e->d_name,"state-",6)) continue;
        int fd=openat(root,e->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if (fd<0) continue;
        char flag;
        if (fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() &&
            flock(fd,LOCK_SH|LOCK_NB)<0 && errno==EWOULDBLOCK && read(fd,&flag,1)==1) {
            if (flag=='R' || flag=='P') result=flag;
        }
        close(fd);
        if (result=='R') break;
    }
    closedir(dir);
    return result;
}

// Композитор готов принять следующий кадр панели
static void panel_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)data;
    (void)time;
    wl_callback_destroy(callback);
    panel.frame = NULL;
}

static const struct wl_callback_listener frame_listener = {
    .done = panel_frame_done,
};

// Нарисовать последние изменения одной группой перед ожиданием событий
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
    cairo_select_font_face(context, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 15);

    draw_button(context, 0, PANEL_MENU_WIDTH, "", false, NULL, NULL);
    draw_menu_icon(context);
    static const char *pinned_icons[PANEL_PIN_COUNT] = {"files", "store", "terminal", "settings"};
    for (int index = 0; index < PANEL_PIN_COUNT; index++) {
        struct panel_window *window = pinned_window(index);
        draw_button(context, PANEL_MENU_WIDTH + index * PANEL_PIN_WIDTH, PANEL_PIN_WIDTH,
            "", window != NULL && window->active, pinned_icons[index], NULL);
    }

    bool showing = false;
    for (struct panel_window *window = panel.windows; window != NULL; window = window->next) {
        showing |= window->hidden_by_desktop;
    }

    int desktop_x = panel.width - PANEL_DESKTOP_WIDTH;
    int clock_x = desktop_x - PANEL_CLOCK_WIDTH;
    int tray_x = clock_x - PANEL_TRAY_WIDTH;
    int keyboard_x = tray_x - PANEL_KEYBOARD_WIDTH;
    int tasks_right = panel.keyboard_available ? keyboard_x : tray_x;
    if (recording) {
        tasks_right -= PANEL_RECORDING_WIDTH;
    }

    draw_button(context, desktop_x, PANEL_DESKTOP_WIDTH, "", showing, "desktop", NULL);
    const int width = PANEL_TASK_WIDTH;
    int x = PANEL_LAUNCHERS_WIDTH;
    for (struct panel_window *window = panel.windows; window != NULL; window = window->next) {
        if (pinned_index(window) >= 0) {
            continue;
        }
        if (x + width > tasks_right) {
            break;
        }

        const char *app_icon = window->app_id != NULL ? window->app_id : "app";
        if (window->title != NULL && strstr(window->title, "Настройки") != NULL) {
            app_icon = "settings";
        }

        draw_button(context, x, width, "", window->active, app_icon, window->icon);
        x += width;
    }

    if (recording) {
        cairo_set_source_rgb(context, 1.0, recording == 'R' ? 0.36 : 0.76, 0.30);
        cairo_arc(context, tasks_right + 12, panel.height / 2, 4, 0, 6.2831853);
        cairo_fill(context);
        cairo_move_to(context, tasks_right + 22, panel.height / 2 + 5);
        cairo_show_text(context, "REC");
    }

    draw_system_tray(context, tray_x);
    if (panel.keyboard_available) {
        draw_button(context, keyboard_x, PANEL_KEYBOARD_WIDTH, "123", false, NULL, NULL);
    }

    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) != NULL) {
        set_panel_color(context, colors->border);
        cairo_rectangle(context, clock_x, 9, 1, panel.height - 18);
        cairo_fill(context);
        set_panel_color(context, colors->text);
        cairo_text_extents_t extents;
        char clock_text[16];
        if (strftime(clock_text, sizeof(clock_text), "%H:%M", &local) != 0) {
            cairo_select_font_face(context, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(context, 16);
            cairo_text_extents(context, clock_text, &extents);
            cairo_move_to(context, clock_x + (PANEL_CLOCK_WIDTH - extents.width) / 2.0,
                panel.height / 2 - 1);
            cairo_show_text(context, clock_text);
        }

        char date_text[16];
        if (strftime(date_text, sizeof(date_text), "%d.%m.%Y", &local) != 0) {
            cairo_select_font_face(context, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(context, 11);
            cairo_text_extents(context, date_text, &extents);
            cairo_move_to(context, clock_x + (PANEL_CLOCK_WIDTH - extents.width) / 2.0,
                panel.height / 2 + 14);
            cairo_show_text(context, date_text);
        }
    }

    cairo_destroy(context);
    cairo_surface_destroy(image);
    wl_buffer_add_listener(buffer->handle, &buffer_listener, buffer);
    panel.frame = wl_surface_frame(panel.surface);
    if (panel.frame != NULL) {
        wl_callback_add_listener(panel.frame, &frame_listener, NULL);
    }
    panel.dirty = false;
    wl_surface_attach(panel.surface, buffer->handle, 0, 0);
    wl_surface_damage_buffer(panel.surface, 0, 0, panel.width, panel.height);
    wl_surface_commit(panel.surface);
}

// Запустить приложение из кнопки панели
static void panel_launch(const char *path, const char *argument) {
    char *const args[] = {(char *)path, (char *)argument, NULL};
    pid_t child;
    int error = posix_spawn(&child, path, NULL, NULL, args, environ);
    if (error != 0) {
        fprintf(stderr, "synapse-panel: %s: %s\n", path, strerror(error));
    }
}

// Открыть или закрыть клавиатуру текущего сеанса
static void toggle_keyboard(void) {
    if (panel.keyboard_pid > 0 && waitpid(panel.keyboard_pid, NULL, WNOHANG) == 0) {
        kill(panel.keyboard_pid, SIGTERM);
        return;
    }

    panel.keyboard_pid = fork();
    if (panel.keyboard_pid == 0) {
        execl("/usr/libexec/labwc/deck-keyboard", "deck-keyboard", "-L", "224", "-H", "240", (char *)NULL);
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

    if (panel.pointer_surface == panel.background_surface) {
        for (int i = 0; i < Shortcuts + panel.desktop_count; i++) {
            int x, y;
            desktop_cell(i, &x, &y);
            if (x + IconWidth > panel.background_width) break;
            if (panel.pointer_x < x || panel.pointer_x >= x + IconWidth - 4 ||
                panel.pointer_y < y || panel.pointer_y >= y + IconHeight - 4) continue;
            if (panel.last_index == i && time - panel.last_click <= 500) {
                panel.last_index = -1;
                if (i == 0) panel_launch("/bin/synapse-files", NULL);
                else if (i == 1) panel_launch("/bin/synapse-menuctl", NULL);
                else if (i == 2) panel_launch("/bin/synapse-desktop", "--settings");
                else {
                    char path[PATH_MAX];
                    if (snprintf(path, sizeof(path), "%s/%s", panel.desktop_dir,
                        panel.desktop_files[i - Shortcuts]->d_name) < (int)sizeof(path)) {
                        panel_launch("/bin/synapse-files", path);
                    }
                }
            } else {
                panel.selected = panel.last_index = i;
                panel.last_click = time;
                background_draw();
            }
            return;
        }
        return;
    }
    if (panel.pointer_surface != panel.surface) return;
    if (panel.pointer_x >= panel.width - PANEL_DESKTOP_WIDTH) {
        bool restore = false;
        struct panel_window *previous_focus = NULL;
        for (struct panel_window *w = panel.windows; w; w = w->next) restore |= w->hidden_by_desktop;
        for (struct panel_window *w = panel.windows; w; w = w->next) {
            if (restore && w->hidden_by_desktop) {
                zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
                if (w->was_active) previous_focus = w;
                w->was_active = w->hidden_by_desktop = false;
            } else if (!restore && !w->minimized) {
                w->hidden_by_desktop = true;
                w->was_active = w->active;
                zwlr_foreign_toplevel_handle_v1_set_minimized(w->handle);
            }
        }
        if (previous_focus) {
            zwlr_foreign_toplevel_handle_v1_activate(previous_focus->handle, panel.seat);
        }
        panel.dirty = true;
    } else if (panel.keyboard_available &&
        panel.pointer_x >= panel.width - PANEL_DESKTOP_WIDTH - PANEL_CLOCK_WIDTH -
        PANEL_TRAY_WIDTH - PANEL_KEYBOARD_WIDTH &&
        panel.pointer_x < panel.width - PANEL_DESKTOP_WIDTH - PANEL_CLOCK_WIDTH - PANEL_TRAY_WIDTH) {
        toggle_keyboard();
    } else if (panel.pointer_x < PANEL_MENU_WIDTH) {
        panel_launch("/bin/synapse-menuctl", NULL);
    } else if (panel.pointer_x < PANEL_LAUNCHERS_WIDTH) {
        static const char *pinned_commands[PANEL_PIN_COUNT] = {
            "/bin/synapse-files", "/bin/synapse-store", "/bin/weston-terminal", "/bin/synapse-desktop"
        };
        int index = (panel.pointer_x - PANEL_MENU_WIDTH) / PANEL_PIN_WIDTH;
        struct panel_window *window = pinned_window(index);
        if (window != NULL) {
            toggle_window(window);
        } else {
            panel_launch(pinned_commands[index], index == 3 ? "--settings" : NULL);
        }
    } else {
        int right = panel.width - PANEL_DESKTOP_WIDTH - PANEL_CLOCK_WIDTH - PANEL_TRAY_WIDTH -
            (panel.keyboard_available ? PANEL_KEYBOARD_WIDTH : 0) -
            (recording ? PANEL_RECORDING_WIDTH : 0);
        const int width = PANEL_TASK_WIDTH;
        int left = PANEL_LAUNCHERS_WIDTH;
        if (panel.pointer_x < left || panel.pointer_x >= right) {
            return;
        }

        int index = (panel.pointer_x - left) / width;
        for (struct panel_window *window = panel.windows; window != NULL; window = window->next) {
            if (pinned_index(window) >= 0) {
                continue;
            }
            if (index-- == 0) {
                toggle_window(window);
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
    panel.pointer_y = wl_fixed_to_int(y);
    panel.pointer_x = wl_fixed_to_int(x);
}

// Принять вход указателя в область панели
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)data;
    (void)pointer;
    (void)serial;
    panel.pointer_surface = surface;
    panel.pointer_y = wl_fixed_to_int(y);
    panel.pointer_x = wl_fixed_to_int(x);
}

// Принять выход указателя из области панели
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface) {
    (void)data;
    (void)pointer;
    (void)serial;
    if (panel.pointer_surface == surface) panel.pointer_surface = NULL;
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

// Сохранить идентификатор для значка и короткого имени окна
static void window_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    const char *app_id) {
    (void)handle;
    if (app_id == NULL) {
        return;
    }
    struct panel_window *window = data;
    char *copy = strdup(app_id);
    if (copy != NULL) {
        cairo_surface_t *icon = load_window_icon(copy);
        if (window->icon != NULL) {
            cairo_surface_destroy(window->icon);
        }
        free(window->app_id);
        window->app_id = copy;
        window->icon = icon;
    }
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
    panel.dirty = true;
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
    free(window->app_id);
    if (window->icon != NULL) {
        cairo_surface_destroy(window->icon);
    }
    free(window);
    panel.dirty = true;
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
    panel.dirty = true;
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
    if (panel.background_committed && panel.background_width == (int)width &&
        panel.background_height == (int)height) {
        return;
    }
    panel.background_committed = false;
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
    panel.keyboard_available = access("/usr/libexec/labwc/deck-keyboard", X_OK) == 0;
    panel.theme = read_theme();
    read_appearance();
    panel.last_index = panel.selected = -1;
    const char *home = getenv("HOME");
    if (home && home[0] == '/' && snprintf(panel.desktop_dir, sizeof(panel.desktop_dir),
        "%s/Desktop", home) < (int)sizeof(panel.desktop_dir)) {
        if (!mkdir(panel.desktop_dir, 0700) || errno == EEXIST) scan_desktop();
    } else {
        panel.desktop_dir[0] = 0;
    }
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
    zwlr_layer_surface_v1_set_size(panel.layer, 0, panel.panel_height);
    zwlr_layer_surface_v1_set_exclusive_zone(panel.layer, panel.panel_height);
    zwlr_layer_surface_v1_add_listener(panel.layer, &layer_listener, NULL);
    wl_surface_commit(panel.surface);
    if (wl_display_roundtrip(panel.display) < 0) {
        return 1;
    }

    time_t last_inspection = (time_t)-1;
    while (wl_display_dispatch_pending(panel.display) >= 0) {
        if (panel.dirty && panel.frame == NULL) {
            panel_draw();
        }
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

        pid_t finished;
        while ((finished = waitpid(-1, NULL, WNOHANG)) > 0) {
            if (finished == panel.keyboard_pid) {
                panel.keyboard_pid = 0;
            }
        }

        time_t now = time(NULL);
        static time_t last_check;
        if (now!=last_check) {
            last_check=now; char active=active_recording();
            if (active!=recording) { recording=active; panel.dirty = true; }
        }
        struct tm local;
        if (localtime_r(&now, &local) != NULL && local.tm_min != panel.last_minute) {
            panel.last_minute = local.tm_min;
            panel.dirty = true;
        }

        if (now == last_inspection) {
            continue;
        }
        last_inspection = now;
        bool desktop_changed = scan_desktop();
        int theme = read_theme();
        if (theme != panel.theme) {
            panel.theme = theme;
            clear_menu_icon();
            desktop_changed = true;
        }
        read_appearance();
        panel.dirty = true;
        const char *home = getenv("HOME");
        char wallpaper_path[512];
        struct stat wallpaper;
        if (home != NULL && snprintf(wallpaper_path, sizeof(wallpaper_path),
            "%s/.config/synapse/wallpaper", home) < (int)sizeof(wallpaper_path) &&
            stat(wallpaper_path, &wallpaper) == 0 && wallpaper.st_mtime != panel.wallpaper_mtime) {
            panel.wallpaper_mtime = wallpaper.st_mtime;
            desktop_changed = true;
        }
        if (desktop_changed) {
            background_draw();
        }
    }

    clear_menu_icon();
    wl_display_disconnect(panel.display);
    return 0;
}
