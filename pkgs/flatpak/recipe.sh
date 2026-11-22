# recipe.sh: собрать CLI Flatpak для SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: сборка менеджера приложений без включения в образ по умолчанию
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 22.11.2026
# Статус доступа: общедоступный

prepare

# Steam создаёт Discord RPC после смены /var/run на symlink, пустой каталог не нужен
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/common/flatpak-exports.c
+++ b/common/flatpak-exports.c
@@ -455,0 +456,5 @@
+
+#ifdef __synapse__
+      if (g_str_has_suffix (path, "/app/com.discordapp.Discord"))
+        continue;
+#endif
PATCH

# Без звукового сервера 32-битный Steam заполняет адресное пространство разделяемой памятью
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/common/flatpak-run-pulseaudio.c
+++ b/common/flatpak-run-pulseaudio.c
@@ -328,2 +328,13 @@
-  else
-    g_info ("Could not find pulseaudio socket");
+  else
+    {
+      g_info ("Could not find pulseaudio socket");
+#ifdef __synapse__
+      static const char config_path[] = "/run/flatpak/pulse/config";
+      static const char client_config[] = "enable-shm=no\nautospawn=no\n";
+
+      flatpak_bwrap_add_args (bwrap, "--dir", "/run/flatpak/pulse", NULL);
+      if (flatpak_bwrap_add_args_data (bwrap, "pulseaudio", client_config,
+                                      -1, config_path, NULL))
+        flatpak_bwrap_set_env (bwrap, "PULSE_CLIENTCONFIG", config_path, TRUE);
+#endif
+    }
PATCH

# Вложенный Steam Runtime хранит файл блокировки в каталоге своего /usr
git -C "$source" apply <<'PATCH'
--- a/common/flatpak-run.c
+++ b/common/flatpak-run.c
@@ -3644,7 +3644,15 @@
     if (runtime_ref_fd >= 0)
       {
         flatpak_bwrap_add_args (bwrap,
                                 "--lock-file", "/usr/.ref",
                                 NULL);
+#ifdef __synapse__
+        if (runtime_fd == custom_runtime_fd &&
+            g_strcmp0 (app_id, "com.valvesoftware.Steam") == 0 &&
+            !flatpak_bwrap_add_args_data_fd_dup (bwrap,
+                                                  "--ro-bind-fd", runtime_ref_fd,
+                                                  "/.ref", error))
+          return FALSE;
+#endif
       }
   }
PATCH

# Прокси нужны каталог сокета и сама сеансовая шина
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/common/flatpak-run-dbus.c
+++ b/common/flatpak-run-dbus.c
@@ -97,1 +97,10 @@
-  flatpak_bwrap_add_args (bwrap, "--bind", proxy_socket_dir, proxy_socket_dir, NULL);
+#ifdef __synapse__
+  g_autofree char *bus_socket = g_build_filename (user_runtime_dir, "bus", NULL);
+  flatpak_bwrap_add_args (bwrap, "--bind", "/tmp", "/tmp", NULL);
+  flatpak_bwrap_add_args (bwrap, "--bind", proxy_socket_dir, proxy_socket_dir, NULL);
+  if (g_file_test (bus_socket, G_FILE_TEST_EXISTS)) {
+    flatpak_bwrap_add_args (bwrap, "--ro-bind", bus_socket, bus_socket, NULL);
+  }
+#else
+  flatpak_bwrap_add_args (bwrap, "--bind", proxy_socket_dir, proxy_socket_dir, NULL);
+#endif
PATCH

# Steam запускает 32-битный клиент через установленный эмулятор
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/common/flatpak-run.c
+++ b/common/flatpak-run.c
@@ -3717,5 +3717,18 @@
     }

+#ifdef __synapse__
+  if (g_strcmp0 (app_id, "com.valvesoftware.Steam") == 0 &&
+      g_file_test ("/bin/qemu-i386", G_FILE_TEST_IS_EXECUTABLE))
+    {
+      flatpak_bwrap_add_args (bwrap,
+                              "--ro-bind", "/bin/qemu-i386", "/run/host/qemu-i386",
+                              NULL);
+      flatpak_bwrap_add_args (bwrap,
+                              "--ro-bind", "/usr/libexec/flatpak/uname-linux", "/run/host/uname",
+                              NULL);
+    }
+#endif
+
   if (original_app_fd >= 0 && original_app_fd != app_fd)
     {
       /* Put the real Flatpak app in /run/parent/app */
PATCH

patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/common/flatpak-run.c
+++ b/common/flatpak-run.c
@@ -3973,1 +3973,12 @@
+#ifdef __synapse__
+  bwrap->envp = g_environ_setenv (bwrap->envp, "SHELL", "/bin/sh", TRUE);
+  if (g_strcmp0 (app_id, "com.valvesoftware.Steam") == 0)
+    {
+      const char *path = g_environ_getenv (bwrap->envp, "PATH");
+      g_autofree char *synapse_path = g_strconcat ("/run/host:",
+                                                   path != NULL ? path : "/app/bin:/usr/bin", NULL);
+      bwrap->envp = g_environ_setenv (bwrap->envp, "PATH", synapse_path, TRUE);
+    }
+#endif
+
   flatpak_bwrap_sort_envp (bwrap);
PATCH

# Steam Pressure Vessel создаёт вложенное пространство пользователей
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/common/flatpak-run.c
+++ b/common/flatpak-run.c
@@ -2383,6 +2383,10 @@
   gboolean parent_expose_pids = (flags & FLATPAK_RUN_FLAG_PARENT_EXPOSE_PIDS) != 0;
   gboolean parent_share_pids = (flags & FLATPAK_RUN_FLAG_PARENT_SHARE_PIDS) != 0;
   gboolean bwrap_unprivileged = flatpak_bwrap_is_unprivileged ();
+#ifdef __synapse__
+  g_autofree char *sandbox_id = app_id_dir != NULL ? g_file_get_basename (app_id_dir) : NULL;
+  gboolean steam_nested_userns = g_strcmp0 (sandbox_id, "com.valvesoftware.Steam") == 0;
+#endif
   gsize i;

   g_return_val_if_fail (runtime_fd >= -1, FALSE);
@@ -2408,1 +2411,8 @@
-          flatpak_bwrap_add_arg (bwrap, "--assert-userns-disabled");
+#ifdef __synapse__
+          if (!steam_nested_userns)
+            {
+              flatpak_bwrap_add_arg (bwrap, "--assert-userns-disabled");
+            }
+#else
+          flatpak_bwrap_add_arg (bwrap, "--assert-userns-disabled");
+#endif
@@ -2415,1 +2422,8 @@
-          flatpak_bwrap_add_arg (bwrap, "--disable-userns");
+#ifdef __synapse__
+          if (!steam_nested_userns)
+            {
+              flatpak_bwrap_add_arg (bwrap, "--disable-userns");
+            }
+#else
+          flatpak_bwrap_add_arg (bwrap, "--disable-userns");
+#endif
PATCH

# Передать ID приложения в дочернюю песочницу Steam
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/common/flatpak-run-private.h
+++ b/common/flatpak-run-private.h
@@ -83,5 +83,6 @@
 gboolean flatpak_run_setup_base_argv (FlatpakBwrap   *bwrap,
                                       int             runtime_fd,
+                                      const char     *app_id,
                                       GFile          *app_id_dir,
                                       const char     *arch,
                                       FlatpakRunFlags flags,
--- a/common/flatpak-run.c
+++ b/common/flatpak-run.c
@@ -2369,5 +2369,6 @@
 flatpak_run_setup_base_argv (FlatpakBwrap   *bwrap,
                              int             runtime_fd,
+                             const char     *app_id,
                              GFile          *app_id_dir,
                              const char     *arch,
                              FlatpakRunFlags flags,
@@ -2385,6 +2386,5 @@
   gboolean bwrap_unprivileged = flatpak_bwrap_is_unprivileged ();
 #ifdef __synapse__
-  g_autofree char *sandbox_id = app_id_dir != NULL ? g_file_get_basename (app_id_dir) : NULL;
-  gboolean steam_nested_userns = g_strcmp0 (sandbox_id, "com.valvesoftware.Steam") == 0;
+  gboolean steam_nested_userns = g_strcmp0 (app_id, "com.valvesoftware.Steam") == 0;
 #endif
   gsize i;
@@ -3824,1 +3824,1 @@
-  if (!flatpak_run_setup_base_argv (bwrap, runtime_fd, app_id_dir, app_arch, flags, error))
+  if (!flatpak_run_setup_base_argv (bwrap, runtime_fd, app_id, app_id_dir, app_arch, flags, error))
--- a/common/flatpak-dir.c
+++ b/common/flatpak-dir.c
@@ -9454,1 +9454,1 @@
-  if (!flatpak_run_setup_base_argv (bwrap, usr_fd, NULL, runtime_arch,
+  if (!flatpak_run_setup_base_argv (bwrap, usr_fd, NULL, NULL, runtime_arch,
--- a/app/flatpak-builtins-build.c
+++ b/app/flatpak-builtins-build.c
@@ -525,1 +525,1 @@
-  if (!flatpak_run_setup_base_argv (bwrap, usr_fd, app_id_dir, arch,
+  if (!flatpak_run_setup_base_argv (bwrap, usr_fd, NULL, app_id_dir, arch,
PATCH

fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"
sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-flatpak-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib/pkgconfig" "$sysroot/usr/bin"
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$sysroot"
ln -s "$sysroot/usr/include/x86_64-linux-gnu/asm" "$sysroot/usr/include/asm"

for package in glib glib-gio libffi pcre2 zlib libcap libarchive libcurl openssl-pic openssl \
	libxml2 libostree json-glib appstream gdk-pixbuf libseccomp gpgme \
	libgpg-error libassuan fuse3 liblzma libe2p libfyaml libxmlb libpng; do
	stage="$BUILD_ROOT/$package/payload/usr"
	[[ -d $stage ]]
	cp -a "$stage/." "$sysroot/usr/"
done

cat >"$sysroot/usr/lib/pkgconfig/zlib.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: zlib
Description: Compression library
Version: 1.3.2
Libs: -L${libdir} -lz
Cflags: -I${includedir}
PC

cat >"$sysroot/usr/lib/pkgconfig/libcrypto.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libcrypto
Description: OpenSSL cryptographic library
Version: 3.5.9
Libs: -L${libdir} -lcrypto
Cflags: -I${includedir}
PC

sed -i \
	-e 's/^Requires: glib-2.0 gio-2.0 gobject-2.0$/Requires: glib-2.0 gio-2.0 gobject-2.0 libcurl libxml-2.0 libfyaml xmlb/' \
	-e '/^Requires.private:/d' "$sysroot/usr/lib/pkgconfig/appstream.pc"
sed -i \
	-e 's/^Requires: gio-unix-2.0$/Requires: gio-unix-2.0 liblzma zlib libcurl gpgme gpg-error libcrypto/' \
	-e '/^Requires.private:/d' "$sysroot/usr/lib/pkgconfig/ostree-1.pc"

for tool in glib-mkenums glib-genmarshal glib-compile-resources gdbus-codegen; do
	ln -s "$(command -v "$tool")" "$sysroot/usr/bin/$tool"
done

printf '\nglib_mkenums=/usr/bin/glib-mkenums\nglib_genmarshal=/usr/bin/glib-genmarshal\n' \
	>>"$sysroot/usr/lib/pkgconfig/glib-2.0.pc"
printf '\nglib_compile_resources=/usr/bin/glib-compile-resources\ngdbus_codegen=/usr/bin/gdbus-codegen\n' \
	>>"$sysroot/usr/lib/pkgconfig/gio-2.0.pc"

cat >"$work/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-O2', '-fPIC', '-D__synapse__', '-isystem', '$(gcc -print-file-name=include)', '-I$sysroot/usr/include']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

meson setup "$work/build" "$source" --wipe --cross-file "$work/cross.ini" \
	--prefix=/usr --bindir=/bin --libdir=lib --sysconfdir=/etc --buildtype=release --wrap-mode=nofallback \
	-Ddefault_library=static -Dtests=false -Dinstalled_tests=false \
	-Dsystem_helper=disabled -Dsystemd=disabled -Ddconf=disabled \
	-Dmalcontent=disabled -Dlibzstd=disabled -Dgir=disabled -Dgtkdoc=disabled \
	-Dman=disabled -Ddocbook_docs=disabled -Dxauth=disabled \
	-Dwayland_security_context=disabled -Dselinux_module=disabled -Dseccomp=disabled \
	-Dsystem_bubblewrap=/bin/bwrap \
	-Dsystem_dbus_proxy=/bin/xdg-dbus-proxy \
	-Dsystem_fusermount=/usr/bin/fusermount3

ninja -C "$work/build" -j"$JOBS" app/flatpak portal/flatpak-portal \
	session-helper/flatpak-session-helper
copy "$work/build/app/flatpak" bin/flatpak
copy "$port/uname-linux.sh" usr/libexec/flatpak/uname-linux
chmod 755 "$payload/usr/libexec/flatpak/uname-linux"
copy "$port/flathub.flatpakrepo" etc/flatpak/remotes.d/flathub.flatpakrepo
chmod 644 "$payload/etc/flatpak/remotes.d/flathub.flatpakrepo"
copy "$work/build/portal/flatpak-portal" usr/libexec/flatpak-portal
copy "$work/build/session-helper/flatpak-session-helper" usr/libexec/flatpak-session-helper
copy "$work/build/portal/org.freedesktop.portal.Flatpak.service" \
	usr/share/dbus-1/services/org.freedesktop.portal.Flatpak.service
copy "$work/build/session-helper/org.freedesktop.Flatpak.service" \
	usr/share/dbus-1/services/org.freedesktop.Flatpak.service
strip --strip-unneeded "$payload/bin/flatpak"
strip --strip-unneeded "$payload/usr/libexec/flatpak-portal" \
	"$payload/usr/libexec/flatpak-session-helper"
copy "$source/COPYING" usr/share/licenses/flatpak/COPYING
