# runtime.sh: загрузка закреплённых библиотек для первого этапа порта Qt
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: графические пакеты
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный, MIT

# Распаковать только данные пакетов, не выполняя установочные сценарии Alpine
qt_import_assets() {
	local filename url sha package cache archive prefix
	local -a filters
	mkdir -p "$work/stage" "$work/metadata" "$BUILD_ROOT/qt6-assets"

	while IFS=$'\t' read -r filename url sha package; do
		cache="$BUILD_ROOT/qt6-assets/$sha.apk"

		(
			exec 8>"$cache.lock"
			flock 8
			fetch "$url" "$sha" "$cache"
		)

		bsdtar --options=read_concatenated_archives -tf "$cache" >"$work/members"
		filters=()

		for prefix in usr/ lib/ etc/fonts/; do
			if grep -q "^$prefix" "$work/members"; then
				filters+=(--include "$prefix*")
			fi
		done

		if ((${#filters[@]} != 0)); then
			bsdtar --options=read_concatenated_archives -xf "$cache" -C "$work/stage" "${filters[@]}"
		fi

		bsdtar --options=read_concatenated_archives -xOf "$cache" .PKGINFO >"$work/metadata/$package.txt"
	done < <(jq -r '.[] | [.filename,.url,.sha256,.name] | @tsv' "$port/assets.json")
}

# Замкнуть библиотеки на musl и адаптированную реализацию Wayland
qt_runtime_dependencies() {
	local destination=$1
	mkdir -p "$work/stage/usr/lib" "$work/stage/lib" \
		"$work/stage/usr/lib/libproxy" "$work/stage/usr/lib/llvm20/lib"
	runtime_deps "$destination" "$work/stage/usr/lib" "$work/stage/lib" \
		"$work/stage/usr/lib/libproxy" "$work/stage/usr/lib/llvm20/lib" \
		"$BUILD_ROOT/qt6-base/payload/usr/lib/qt6/runtime" \
		"$BUILD_ROOT/qt6-llvm-runtime/payload/usr/lib/qt6/llvm" \
		"$BUILD_ROOT/weston/payload/usr/lib/weston/runtime"
}

# Убрать только одинаковые копии, которые уже поставляет зависимость пакета
qt_remove_provided() {
	local directory=$1 provided=$2 file basename

	while IFS= read -r -d '' file; do
		basename=$(basename "$file")

		if [[ -f $provided/$basename ]]; then
			if cmp -s "$file" "$provided/$basename"; then
				rm -- "$file"
			else
				printf 'Qt runtime conflicts with its dependency: %s\n' "$basename" >&2
				return 1
			fi
		fi
	done < <(find "$directory" -maxdepth 1 -type f -print0)
}

# Сохранить сведения об источнике, лицензиях и сборке использованных APK
qt_license_metadata() {
	local file

	for file in "$work/metadata/"*.txt; do
		copy "$file" "usr/share/licenses/$name/apk/$(basename "$file")"
	done

	copy "$port/assets.json" "usr/share/licenses/$name/assets.json"
}
