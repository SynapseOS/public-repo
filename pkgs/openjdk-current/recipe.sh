# recipe.sh: Этот пакет задаёт стабильную зависимость от поддерживаемой Java
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

java_lib=usr/lib/jvm/java-21-openjdk/lib
pinned="$ROOT/pkgs/openjdk/port.json"
for asset in $(jq -r '.desktop_assets | keys[]' "$pinned"); do
    entry=".desktop_assets[$asset]"
    url=$(jq -er "$entry.url" "$pinned")
    sha=$(jq -er "$entry.sha256" "$pinned")
    archive="$work/downloads/${url##*/}"
    fetch "$url" "$sha" "$archive"
    mkdir -p "$work/source"
    tar --warning=no-unknown-keyword --ignore-zeros -xzf "$archive" -C "$work/source" --wildcards 'usr/lib/*'
    while IFS= read -r file; do
        copy "$work/source/$file" "$java_lib/${file##*/}"
        chmod 644 "$payload/$java_lib/${file##*/}"
    done < <(jq -er "$entry.files[]" "$pinned")
done

mkdir -p "$payload/usr/share/doc/openjdk-current"
printf 'OpenJDK 21 with AWT and Swing via X11/XWayland. Run java -version.\n' \
    > "$payload/usr/share/doc/openjdk-current/README.md"
