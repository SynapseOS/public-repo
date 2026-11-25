// launch.c: Запуск Labwc с его библиотеками
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и Linux
// Описание: Передать управление композитору без зависимости от командной оболочки
// Подсистема: графическая оболочка
// Первое изменение: 28.10.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный

#include "posix-profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <amdgpu_drm.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Записать весь обновлённый файл настройки
static bool write_all(int fd, const char *data, size_t length) {
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, data + written, length - written);
        if (count <= 0) {
            return false;
        }

        written += (size_t)count;
    }

    return true;
}

// Заменить только прежнюю команду Super в личной настройке Labwc
static void migrate_super_menu(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == 0) {
        return;
    }

    char path[512];
    if (snprintf(path, sizeof(path), "%s/.config/labwc/rc.xml", home) >= (int)sizeof(path)) {
        return;
    }

    int source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) {
        return;
    }

    struct stat attributes;
    if (fstat(source, &attributes) != 0 || !S_ISREG(attributes.st_mode) ||
        attributes.st_uid != getuid() || attributes.st_size <= 0 ||
        attributes.st_size > 65536) {
        close(source);
        return;
    }

    size_t length = (size_t)attributes.st_size;
    char *config = malloc(length + 1);
    if (config == NULL) {
        close(source);
        return;
    }

    size_t received = 0;
    while (received < length) {
        ssize_t count = read(source, config + received, length - received);
        if (count <= 0) {
            free(config);
            close(source);
            return;
        }

        received += (size_t)count;
    }

    close(source);
    config[length] = 0;

    struct menu_binding {
        const char *old_text;
        const char *new_text;
    };
    const struct menu_binding bindings[] = {
        {"command=\"/bin/labwc --show-menu\"", "command=\"synapse-desktop\""},
        {"command=\"labwc --show-menu\"", "command=\"synapse-desktop\""},
        {"<action name=\"ShowMenu\" menu=\"root-menu\" />",
            "<action name=\"Execute\" command=\"synapse-desktop\" />"},
        {"<action name=\"ShowMenu\" menu=\"root-menu\"/>",
            "<action name=\"Execute\" command=\"synapse-desktop\" />"},
    };
    const char *old = NULL;
    const char *replacement = NULL;
    size_t old_length = 0;
    const char *binding = config;
    while ((binding = strstr(binding, "<keybind key=\"Super_L\"")) != NULL) {
        const char *end = strstr(binding, "</keybind>");
        if (end == NULL) {
            break;
        }

        for (size_t index = 0; index < sizeof(bindings) / sizeof(bindings[0]); index++) {
            const char *found = strstr(binding, bindings[index].old_text);
            if (found != NULL && found < end) {
                old = found;
                old_length = strlen(bindings[index].old_text);
                replacement = bindings[index].new_text;
                break;
            }
        }

        if (old != NULL) {
            break;
        }

        binding = end + strlen("</keybind>");
    }

    if (old == NULL) {
        free(config);
        return;
    }

    size_t before = (size_t)(old - config);
    size_t replacement_length = strlen(replacement);
    size_t new_length = before + replacement_length + length - before - old_length;
    char *updated = malloc(new_length + 1);
    if (updated == NULL) {
        free(config);
        return;
    }

    memcpy(updated, config, before);
    memcpy(updated + before, replacement, replacement_length);
    memcpy(updated + before + replacement_length, old + old_length,
        length - before - old_length);
    updated[new_length] = 0;
    free(config);

    char temporary[528];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp-XXXXXX", path) >=
        (int)sizeof(temporary)) {
        free(updated);
        return;
    }

    int target = mkstemp(temporary);
    if (target >= 0) {
        bool saved = fchmod(target, attributes.st_mode & 0777) == 0 &&
            write_all(target, updated, new_length) && fsync(target) == 0;
        if (close(target) != 0) {
            saved = false;
        }

        struct stat current;
        if (lstat(path, &current) != 0 || !S_ISREG(current.st_mode) ||
            current.st_dev != attributes.st_dev || current.st_ino != attributes.st_ino ||
            current.st_size != attributes.st_size ||
            current.st_mtim.tv_sec != attributes.st_mtim.tv_sec ||
            current.st_mtim.tv_nsec != attributes.st_mtim.tv_nsec) {
            saved = false;
        }

        if (!saved || rename(temporary, path) != 0) {
            unlink(temporary);
        }
    }

    free(updated);
}

// Выполнить служебную команду и дождаться результата
static bool run_tool(const char *path, char *const arguments[]) {
    pid_t child = fork();
    if (child < 0) {
        return false;
    }

    if (child == 0) {
        execv(path, arguments);
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Открыть сеансовую шину для приложений Labwc
static void prepare_session_bus(const char *runtime) {
    if (access("/bin/dbus-daemon", X_OK) != 0 || access("/bin/dbus-uuidgen", X_OK) != 0) {
        return;
    }

    if (geteuid() == 0 && access("/etc/machine-id", R_OK) != 0) {
        char *const generate[] = {"dbus-uuidgen", "--ensure=/etc/machine-id", NULL};
        if (!run_tool("/bin/dbus-uuidgen", generate)) {
            return;
        }
    }

    char socket_path[256];
    char address[272];
    char option[288];
    if (snprintf(socket_path, sizeof(socket_path), "%s/bus", runtime) >= (int)sizeof(socket_path) ||
        snprintf(address, sizeof(address), "unix:path=%s", socket_path) >= (int)sizeof(address) ||
        snprintf(option, sizeof(option), "--address=%s", address) >= (int)sizeof(option)) {
        return;
    }

    if (access(socket_path, F_OK) != 0) {
        char *const daemon[] = {"dbus-daemon", "--session", "--fork", option, NULL};
        if (!run_tool("/bin/dbus-daemon", daemon)) {
            return;
        }
    }

    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
}

// Запустить совместимый с Flatpak звуковой сервер при наличии устройства
static void prepare_audio(const char *runtime) {
    if (access("/usr/bin/pulseaudio", X_OK) != 0 || access("/dev/dsp", F_OK) != 0) {
        return;
    }

    char socket_path[256];
    char server[272];
    char module[320];
    if (snprintf(socket_path, sizeof(socket_path), "%s/pulse/native", runtime) >=
            (int)sizeof(socket_path) ||
        snprintf(server, sizeof(server), "unix:%s", socket_path) >= (int)sizeof(server) ||
        snprintf(module, sizeof(module),
            "module-native-protocol-unix socket=%s auth-anonymous=1", socket_path) >=
            (int)sizeof(module)) {
        return;
    }

    if (setenv("PULSE_SERVER", server, 1) != 0 || access(socket_path, F_OK) == 0) {
        return;
    }

    pid_t child = fork();
    if (child != 0) {
        return;
    }

    setsid();
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) {
            close(null_fd);
        }
    }

    char *const arguments[] = {
        "pulseaudio", "-n", "--daemonize=no", "--exit-idle-time=-1", "--log-level=warn",
        "-L", module, "-L", "module-oss device=/dev/dsp mmap=0 record=0 rate=48000 channels=2 format=s16le",
        "-L", "module-suspend-on-idle timeout=2", NULL,
    };
    execv("/usr/bin/pulseaudio", arguments);
    _exit(127);
}

// Добавить ярлыки Flatpak к каталогам данных рабочего стола
static bool prepare_data_dirs(void) {
    const char *current = getenv("XDG_DATA_DIRS");
    if (current != NULL && strstr(current, "/var/lib/flatpak/exports/share") != NULL) {
        return true;
    }

    if (current == NULL || current[0] == 0) {
        current = "/usr/local/share:/usr/share";
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == 0) {
        home = "/root";
    }

    char paths[1024];
    if (snprintf(paths, sizeof(paths), "%s/.local/share/flatpak/exports/share:"
            "/var/lib/flatpak/exports/share:%s", home, current) >= (int)sizeof(paths)) {
        return false;
    }

    return setenv("XDG_DATA_DIRS", paths, 1) == 0;
}

// Проверить наличие очереди графических команд, помимо доступности узла DRM
static bool supports_graphics(const char *device) {
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    char name[32] = {0};
    struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
    bool graphics = true;
    if (ioctl(fd, DRM_IOCTL_VERSION, &version) == 0 && strcmp(name, "amdgpu") == 0) {
        struct drm_amdgpu_info_hw_ip info = {0};
        struct drm_amdgpu_info query = {
            .return_pointer = (uintptr_t)&info,
            .return_size = sizeof(info),
            .query = AMDGPU_INFO_HW_IP_INFO,
            .query_hw_ip = {.type = AMDGPU_HW_IP_GFX},
        };
        graphics = ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &query) == 0 && info.available_rings != 0;
        if (!graphics) {
            fputs("labwc: AMDGPU graphics rings unavailable, using software rendering\n", stderr);
        }
    }
    close(fd);
    return graphics;
}

// Подготовить прямой вывод на экран, когда родительского Wayland-сеанса нет
static bool prepare_native_session(char *runtime, size_t capacity) {
    if (getenv("WAYLAND_DISPLAY") != NULL || getenv("WAYLAND_SOCKET") != NULL) {
        return true;
    }

    if (getuid() == 0) {
        if (snprintf(runtime, capacity, "/var/run/labwc-runtime") >= (int)capacity) {
            return false;
        }
    } else if (snprintf(runtime, capacity, "/tmp/labwc-runtime-%u", (unsigned)getuid()) >=
        (int)capacity) {
        return false;
    }

    if (mkdir(runtime, 0700) != 0 && errno != EEXIST) {
        return false;
    }

    struct stat attributes;
    if (stat(runtime, &attributes) != 0 || !S_ISDIR(attributes.st_mode) ||
        attributes.st_uid != getuid() || (attributes.st_mode & 0777) != 0700) {
        return false;
    }

    if (getenv("XCURSOR_THEME") == NULL &&
        setenv("XCURSOR_THEME", "breeze_cursors", 1) != 0) {
        return false;
    }

    const char *render_device = getenv("WLR_RENDER_DRM_DEVICE");
    if (!render_device || !*render_device) {
        render_device = "/dev/dri/renderD128";
    }
    bool render_node = access(render_device, R_OK | W_OK) == 0;
    const char *renderer = getenv("WLR_RENDERER");

    if (renderer == NULL || renderer[0] == 0) {
        if (getenv("SYNAPSE_LABWC_GLES2") != NULL) {
            renderer = "gles2";
        } else if (render_node && supports_graphics(render_device)) {
            // wlroots вернётся к Pixman, если не сможет создать контекст EGL
            renderer = "auto";
        } else {
            renderer = "pixman";
        }
    }

    bool needs_render_node = strcmp(renderer, "auto") == 0 || strcmp(renderer, "gles2") == 0;
    if (needs_render_node && render_node &&
        getenv("WLR_RENDER_DRM_DEVICE") == NULL &&
        setenv("WLR_RENDER_DRM_DEVICE", "/dev/dri/renderD128", 1) != 0) {
        return false;
    }

    if (needs_render_node && render_node &&
        getenv("SYNAPSE_FB_DIRECT_SCANOUT") == NULL &&
        setenv("SYNAPSE_FB_DIRECT_SCANOUT", "1", 1) != 0) {
        return false;
    }

    return setenv("XDG_RUNTIME_DIR", runtime, 1) == 0 &&
        setenv("WLR_BACKENDS", "headless", 1) == 0 &&
        setenv("WLR_RENDERER", renderer, 1) == 0 &&
        setenv("WLR_HEADLESS_OUTPUTS", "1", 1) == 0 &&
        setenv("SYNAPSE_FB", "1", 1) == 0;
}

// Добавить путь к библиотекам и передать аргументы композитору
int main(int argc, char **argv) {
    (void)argc;

    bool native = getenv("WAYLAND_DISPLAY") == NULL && getenv("WAYLAND_SOCKET") == NULL;
    char runtime[256] = "";

    if (getenv("XKB_DEFAULT_LAYOUT") == NULL &&
        setenv("XKB_DEFAULT_LAYOUT", "us,ru", 1) != 0) {
        perror("labwc: keyboard layouts");
        return 1;
    }

    if (getenv("XKB_DEFAULT_OPTIONS") == NULL &&
        setenv("XKB_DEFAULT_OPTIONS", "grp:alt_shift_toggle", 1) != 0) {
        perror("labwc: keyboard switch");
        return 1;
    }

    if (setenv("LD_LIBRARY_PATH", "/usr/lib/labwc/runtime:/usr/lib/weston/runtime:/usr/lib", 1) != 0) {
        perror("labwc: LD_LIBRARY_PATH");
        return 1;
    }

    if (setenv("WLR_XWAYLAND", "/bin/Xwayland", 1) != 0) {
        perror("labwc: WLR_XWAYLAND");
        return 1;
    }

    if (getenv("LIBGL_DRIVERS_PATH") == NULL &&
        setenv("LIBGL_DRIVERS_PATH", "/usr/lib/dri", 1) != 0) {
        perror("labwc: LIBGL_DRIVERS_PATH");
        return 1;
    }

    if (setenv("LABWC_UPDATE_ACTIVATION_ENV", "1", 1) != 0) {
        perror("labwc: activation environment");
        return 1;
    }

    if (!prepare_native_session(runtime, sizeof(runtime))) {
        perror("labwc: native session");
        return 1;
    }

    if (!prepare_data_dirs()) {
        fputs("labwc: XDG_DATA_DIRS unavailable\n", stderr);
        return 1;
    }

    if (native) {
        if (!geteuid() && prepare_posix_profile(getuid(), getgid())) perror("POSIX profile");
        apply_posix_profile(getenv("HOME"));
    }
    migrate_super_menu();

    if (native) {
        prepare_session_bus(runtime);
        prepare_audio(runtime);
    }

    argv[0] = (char *)"/usr/libexec/labwc/labwc";
    execv(argv[0], argv);

    perror("labwc: execv");
    return 127;
}
