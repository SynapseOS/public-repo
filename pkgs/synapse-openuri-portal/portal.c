// portal.c: Портал открытия ссылок для приложений Flatpak
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и Linux
// Описание: передать веб-ссылку браузеру и ссылку входа GitHub Desktop
// Подсистема: графический сеанс
// Первое изменение: 29.10.2026
// Последнее изменение: 30.10.2026
// Доступ: общедоступный

#include <gio/gio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const gchar portal_xml[] =
    "<node><interface name='org.freedesktop.portal.OpenURI'>"
    "<method name='OpenURI'><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='a{sv}' direction='in'/>"
    "<arg type='o' direction='out'/></method>"
    "<property name='version' type='u' access='read'/>"
    "</interface>"
    "<interface name='org.freedesktop.portal.Settings'>"
    "<method name='ReadAll'><arg type='as' direction='in'/>"
    "<arg type='a{sa{sv}}' direction='out'/></method>"
    "<method name='Read'><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<method name='ReadOne'><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<signal name='SettingChanged'><arg type='s'/><arg type='s'/>"
    "<arg type='v'/></signal>"
    "<property name='version' type='u' access='read'/>"
    "</interface></node>";

static guint last_color_scheme;

// Прочитать предпочтение оформления текущего пользователя
static guint read_color_scheme(void) {
    const char *home = g_get_home_dir();
    if (home == NULL || home[0] == 0) {
        home = "/root";
    }

    gchar *user_path = g_build_filename(home, ".config", "labwc", "rc.xml", NULL);
    const char *path = g_file_test(user_path, G_FILE_TEST_IS_REGULAR) ? user_path :
        "/etc/xdg/labwc/rc.xml";
    gchar *config = NULL;

    if (!g_file_get_contents(path, &config, NULL, NULL)) {
        g_free(user_path);
        return 1;
    }

    guint scheme = strstr(config, "<name>Arc-Clone</name>") != NULL ? 2 : 1;
    g_free(config);
    g_free(user_path);
    return scheme;
}

// Сообщить приложениям об изменении светлой или тёмной темы
static gboolean refresh_color_scheme(gpointer data) {
    GDBusConnection *bus = data;
    guint scheme = read_color_scheme();
    if (scheme != last_color_scheme) {
        last_color_scheme = scheme;
        g_dbus_connection_emit_signal(bus, NULL, "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Settings", "SettingChanged",
            g_variant_new("(ssv)", "org.freedesktop.appearance", "color-scheme",
                g_variant_new_uint32(scheme)), NULL);
    }

    return G_SOURCE_CONTINUE;
}

typedef struct {
    GDBusConnection *bus;
    gchar *path;
    guint registration;
    guint result;
} PortalRequest;

// Проверить ссылку и выбрать зарегистрированное приложение
static const char *application_for_uri(const char *uri) {
    if (g_str_has_prefix(uri, "https://") || g_str_has_prefix(uri, "http://")) {
        return "org.mozilla.firefox";
    }

    if (g_str_has_prefix(uri, "x-github-client://") ||
        g_str_has_prefix(uri, "x-github-desktop-auth://") ||
        g_str_has_prefix(uri, "x-github-desktop-dev-auth://")) {
        return "io.github.shiftey.Desktop";
    }

    return NULL;
}

// Запустить обработчик ссылки без командной оболочки
static gboolean launch_uri(const char *application, const char *uri) {
    pid_t child = fork();
    if (child < 0) {
        return FALSE;
    }

    if (child == 0) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (runtime != NULL && getenv("WAYLAND_DISPLAY") == NULL) {
            char socket_path[512];
            if (g_snprintf(socket_path, sizeof(socket_path), "%s/wayland-0", runtime) <
                (int)sizeof(socket_path) && access(socket_path, F_OK) == 0) {
                setenv("WAYLAND_DISPLAY", "wayland-0", 1);
            }
        }

        execl("/bin/flatpak", "flatpak", "run", application, uri, (char *)NULL);
        _exit(127);
    }

    return TRUE;
}

// Завершить запрос после ответа клиенту D-Bus
static gboolean complete_request(gpointer data) {
    PortalRequest *request = data;
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));

    g_dbus_connection_emit_signal(request->bus, NULL, request->path,
        "org.freedesktop.portal.Request", "Response",
        g_variant_new("(u@a{sv})", request->result, g_variant_builder_end(&results)), NULL);

    g_dbus_connection_unregister_object(request->bus, request->registration);
    g_object_unref(request->bus);
    g_free(request->path);
    g_free(request);
    return G_SOURCE_REMOVE;
}

// Проверить имя объекта запроса, полученное от клиента
static gboolean valid_token(const char *token) {
    if (token == NULL || token[0] == 0 || strlen(token) > 128) {
        return FALSE;
    }

    for (const char *position = token; *position != 0; position++) {
        if (!g_ascii_isalnum(*position) && *position != '_') {
            return FALSE;
        }
    }

    return TRUE;
}

// Построить путь запроса по адресу клиента D-Bus
static gchar *request_path(const char *sender, const char *token) {
    const char *unique_name = sender != NULL ? sender : "unknown";
    if (unique_name[0] == ':') {
        unique_name++;
    }

    gchar *name = g_strdup(unique_name);
    for (char *position = name; *position != 0; position++) {
        if (!g_ascii_isalnum(*position)) {
            *position = '_';
        }
    }

    gchar *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", name, token);
    g_free(name);
    return path;
}

// Обработать запрос открытия ссылки из песочницы
static void call_method(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *method, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer data) {
    (void)path;
    (void)interface;
    (void)data;

    if (g_strcmp0(method, "OpenURI") != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
        return;
    }

    const char *parent;
    const char *uri;
    GVariant *options;
    g_variant_get(parameters, "(&s&s@a{sv})", &parent, &uri, &options);
    (void)parent;

    const char *application = application_for_uri(uri);
    if (application == NULL || strlen(uri) > 4096) {
        g_variant_unref(options);
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.portal.Error.InvalidArgument", "Unsupported URI");
        return;
    }

    const char *token = NULL;
    g_variant_lookup(options, "handle_token", "&s", &token);
    gchar *fallback = NULL;
    if (!valid_token(token)) {
        fallback = g_strdup_printf("synapse_%u", g_random_int());
        token = fallback;
    }

    PortalRequest *request = g_new0(PortalRequest, 1);
    request->bus = g_object_ref(bus);
    request->path = request_path(sender, token);
    request->result = launch_uri(application, uri) ? 0 : 2;

    static const GDBusInterfaceVTable empty_vtable = {0};
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.freedesktop.portal.Request'>"
        "<method name='Close'/><signal name='Response'>"
        "<arg type='u'/><arg type='a{sv}'/></signal></interface></node>", NULL);
    request->registration = g_dbus_connection_register_object(bus, request->path,
        node->interfaces[0], &empty_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node);

    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request->path));
    g_idle_add(complete_request, request);
    g_free(fallback);
    g_variant_unref(options);
}

// Вернуть поддерживаемые системные настройки графическим приложениям
static void call_settings_method(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *method, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer data) {
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)data;

    if (g_strcmp0(method, "ReadAll") == 0) {
        GVariantIter *namespaces = NULL;
        const gchar *name;
        gboolean include = FALSE;
        g_variant_get(parameters, "(as)", &namespaces);

        if (g_variant_iter_n_children(namespaces) == 0) {
            include = TRUE;
        }

        while (g_variant_iter_loop(namespaces, "&s", &name)) {
            if (name[0] == '\0' || g_strcmp0(name, "*") == 0 ||
                g_strcmp0(name, "org.freedesktop.*") == 0 ||
                g_strcmp0(name, "org.freedesktop.appearance") == 0) {
                include = TRUE;
            }
        }

        g_variant_iter_free(namespaces);

        GVariantBuilder values;
        g_variant_builder_init(&values, G_VARIANT_TYPE("a{sa{sv}}"));
        if (include) {
            GVariantBuilder appearance;
            g_variant_builder_init(&appearance, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&appearance, "{sv}", "color-scheme",
                g_variant_new_uint32(read_color_scheme()));
            g_variant_builder_add(&values, "{s@a{sv}}", "org.freedesktop.appearance",
                g_variant_builder_end(&appearance));
        }

        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@a{sa{sv}})", g_variant_builder_end(&values)));
        return;
    }

    if (g_strcmp0(method, "Read") == 0 || g_strcmp0(method, "ReadOne") == 0) {
        const gchar *name;
        const gchar *key;
        g_variant_get(parameters, "(&s&s)", &name, &key);

        if (g_strcmp0(name, "org.freedesktop.appearance") == 0 &&
            g_strcmp0(key, "color-scheme") == 0) {
            GVariant *value = g_variant_new_uint32(read_color_scheme());
            if (g_strcmp0(method, "Read") == 0) {
                value = g_variant_new_variant(value);
            }

            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
            return;
        }

        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.portal.Error.NotFound", "Unknown setting");
        return;
    }

    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
}

// Предоставить номер версии портала клиенту
static GVariant *read_property(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *property, GError **error, gpointer data) {
    (void)bus;
    (void)sender;
    (void)path;
    (void)data;

    if (g_strcmp0(property, "version") == 0) {
        return g_variant_new_uint32(g_strcmp0(interface, "org.freedesktop.portal.Settings") == 0 ? 2 : 1);
    }

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
    return NULL;
}

// Зарегистрировать OpenURI в пользовательской шине
int main(void) {
    signal(SIGCHLD, SIG_IGN);

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (bus == NULL) {
        g_printerr("OpenURI: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(portal_xml, &error);
    if (node == NULL) {
        g_printerr("OpenURI: %s\n", error->message);
        g_error_free(error);
        g_object_unref(bus);
        return 1;
    }

    static const GDBusInterfaceVTable vtable = {
        .method_call = call_method,
        .get_property = read_property,
    };
    static const GDBusInterfaceVTable settings_vtable = {
        .method_call = call_settings_method,
        .get_property = read_property,
    };
    guint registration = g_dbus_connection_register_object(bus,
        "/org/freedesktop/portal/desktop", node->interfaces[0], &vtable, NULL, NULL, &error);
    if (registration == 0) {
        g_printerr("OpenURI: %s\n", error->message);
        g_error_free(error);
        g_dbus_node_info_unref(node);
        g_object_unref(bus);
        return 1;
    }

    registration = g_dbus_connection_register_object(bus,
        "/org/freedesktop/portal/desktop", node->interfaces[1], &settings_vtable, NULL, NULL, &error);
    g_dbus_node_info_unref(node);
    if (registration == 0) {
        g_printerr("Settings: %s\n", error->message);
        g_error_free(error);
        g_object_unref(bus);
        return 1;
    }

    GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.portal.Desktop", 4u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (reply == NULL) {
        g_printerr("OpenURI: %s\n", error->message);
        g_error_free(error);
        g_object_unref(bus);
        return 1;
    }

    guint result = 0;
    g_variant_get(reply, "(u)", &result);
    g_variant_unref(reply);
    if (result != 1 && result != 4) {
        g_printerr("OpenURI: bus name unavailable\n");
        g_object_unref(bus);
        return 1;
    }

    last_color_scheme = read_color_scheme();
    g_timeout_add_seconds(2, refresh_color_scheme, bus);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    g_object_unref(bus);
    return 0;
}
