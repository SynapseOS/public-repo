# recipe.sh: собрать Bubblewrap без набора привилегий хоста
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: подготовить bwrap для гостевого теста пространств имён
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

prepare
export SYNAPSE_LINK_MODE=dynamic

# Отдельное пространство сети SynapseOS уже закрывает весь IPv4
# Настройка Linux loopback через NETLINK_ROUTE здесь неприменима
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/bubblewrap.c
+++ b/bubblewrap.c
@@ -3247,7 +3247,9 @@
   switch_to_user_with_privs ();

+#ifndef __synapse__
   if (opt_unshare_net)
     loopback_setup (); /* Will exit if unsuccessful */
+#endif

   ns_uid = opt_sandbox_uid;
   ns_gid = opt_sandbox_gid;
PATCH

# В нашем VFS mount привязан к узлу, поэтому новый tmpfs может скрыть путь
# источника и в oldroot. Сохраняем FD источников до монтирования tmpfs
patch --batch --fuzz=0 -d "$source" -p1 <<'PATCH'
--- a/bubblewrap.c
+++ b/bubblewrap.c
@@ -158,6 +158,9 @@ struct _SetupOp
   const char *source;
   const char *dest;
   int         fd;
+#ifdef __synapse__
+  int         synapse_source_fd;
+#endif
   SetupOpFlag flags;
   int         perms;
   size_t      size;  /* number of bytes, zero means unset/default */
@@ -217,6 +220,9 @@ setup_op_new (SetupOpType type)

   op->type = type;
   op->fd = -1;
+#ifdef __synapse__
+  op->synapse_source_fd = -1;
+#endif
   op->flags = 0;
   return op;
 }
@@ -1155,6 +1161,11 @@ setup_newroot (bool unshare_pid)
       if (op->source &&
           op->type != SETUP_MAKE_SYMLINK)
         {
+#ifdef __synapse__
+          if (op->synapse_source_fd >= 0)
+            source_fd = dup (op->synapse_source_fd);
+          else
+#endif
           if (has_path_prefix (op->source, "/proc"))
             {
               /* openat_in_root ("/oldroot") fails for some /proc magic links, such as
@@ -1627,4 +1640,11 @@ close_ops_fd (void)
           op->fd = -1;
         }
+#ifdef __synapse__
+      if (op->synapse_source_fd >= 0)
+        {
+          (void) close (op->synapse_source_fd);
+          op->synapse_source_fd = -1;
+        }
+#endif
     }
 }
@@ -1676,6 +1692,10 @@ resolve_symlinks_in_ops (void)
         case SETUP_OVERLAY_SRC:
         case SETUP_OVERLAY_MOUNT:
           old_source = op->source;
+#ifdef __synapse__
+          if (has_path_prefix (old_source, "/proc/self/fd"))
+            break;
+#endif
           op->source = realpath (old_source, NULL);
           if (op->source == NULL)
             {
@@ -3278,6 +3298,18 @@
   /* Need to do this before the chroot, but after we're the real uid */
   resolve_symlinks_in_ops ();

+#ifdef __synapse__
+  for (SetupOp *op = ops; op != NULL; op = op->next)
+    {
+      if (op->source != NULL && op->type != SETUP_MAKE_SYMLINK &&
+          (!has_path_prefix (op->source, "/proc") ||
+           has_path_prefix (op->source, "/proc/self/fd")))
+        {
+          op->synapse_source_fd = open (op->source, O_PATH | O_CLOEXEC);
+        }
+    }
+#endif
+
   /* Mark everything as slave, so that we still
    * receive mounts from the real root, but don't
    * propagate mounts to the real root. */
PATCH

fetch "$(jq -er .linux_headers_source "$spec")" \
	"$(jq -er .linux_headers_sha256 "$spec")" "$work/linux-libc-dev.deb"

sysroot=$(mktemp -d "${TMPDIR:-/tmp}/synapse-bwrap-sysroot.XXXXXX")
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include/sys" "$sysroot/usr/include/linux" \
	"$sysroot/usr/lib/pkgconfig"
ar p "$work/linux-libc-dev.deb" data.tar.xz | tar -xJf - -C "$sysroot"
ln -s "$sysroot/usr/include/x86_64-linux-gnu/asm" "$sysroot/usr/include/asm"
cp "$BUILD_ROOT/libcap/payload/usr/include/sys/capability.h" "$sysroot/usr/include/sys/"
cp "$BUILD_ROOT/libcap/payload/usr/include/linux/capability.h" "$sysroot/usr/include/linux/"
cp "$BUILD_ROOT/libcap/payload/usr/lib/libcap.a" "$sysroot/usr/lib/"
cp "$BUILD_ROOT/libcap/payload/usr/lib/pkgconfig/libcap.pc" "$sysroot/usr/lib/pkgconfig/"

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
c_args = ['-O2', '-D__synapse__', '-I$sysroot/usr/include']
c_link_args = ['-L$sysroot/usr/lib']
CROSS

export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"

rm -rf -- "$work/build"
meson setup "$work/build" "$source" --cross-file "$work/cross.ini" \
	--prefix=/usr --bindir=/bin --libdir=lib --buildtype=release \
	-Dtests=false -Dman=disabled -Dselinux=disabled \
	-Dbash_completion=disabled -Dzsh_completion=disabled

ninja -C "$work/build" -j"$JOBS"
copy "$work/build/bwrap" bin/bwrap
copy "$source/COPYING.LIB" usr/share/licenses/bubblewrap/COPYING.LIB
