# qmi-meson.sh: общая кросс-сборка GLib-зависимых пакетов модема
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64 / ARM64, ABI: synapse-x86_64-v1 / synapse-aarch64-v1
# Подсистема: сборка пакетов
# Первое изменение: 25.11.2026, последнее изменение: 25.11.2026
# Статус доступа: общедоступный

# Изолировать pkg-config от библиотек ноутбука и собрать пути готовых зависимостей.
qmi_cross_setup() {
	local dependency cpu=${TARGET_ARCH:-x86_64}
	qmi_deps="$work/dependencies"
	rm -rf -- "$qmi_deps"
	mkdir -p "$qmi_deps/usr/lib/pkgconfig"
	for dependency in pcre2 libffi zlib glib glib-gio "$@"; do
		test -d "$BUILD_ROOT/$dependency/payload/usr"
		cp -a "$BUILD_ROOT/$dependency/payload/usr/." "$qmi_deps/usr/"
	done
	export PKG_CONFIG_LIBDIR="$qmi_deps/usr/lib/pkgconfig"
	export PKG_CONFIG_SYSROOT_DIR="$qmi_deps"
	cat >"$work/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = '$cpu'
cpu = '$cpu'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-O2', '-D__synapse__']
c_link_args = ['-static']
CROSS
}

# Скопировать установленные файлы, сохранив только поддерживаемые обычные файлы.
qmi_copy_install() {
	local file relative
	while IFS= read -r -d '' file; do
		relative=${file#"$work/install/"}
		case "$relative" in
			usr/bin/*) relative="bin/${relative#usr/bin/}" ;;
		esac
		copy "$file" "$relative"
		if [[ -n ${QMI_STRIP:-} && -x $file ]] && readelf -h "$file" >/dev/null 2>&1; then
			"$QMI_STRIP" --strip-debug "$payload/$relative"
		fi
	done < <(find "$work/install" -type f -print0)
}
