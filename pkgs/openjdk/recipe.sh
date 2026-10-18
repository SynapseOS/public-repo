# recipe.sh: Упаковать закреплённый musl runtime и доверенные сертификаты
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

mkdir -p "$work/source"

while IFS=$'\t' read -r filename url sha; do
	fetch "$url" "$sha" "$work/downloads/$filename.apk"
	bsdtar --options=read_concatenated_archives -xf "$work/downloads/$filename.apk" -C "$work/source" --include 'usr/*'
done < <(jq -r '.assets[] | [.name,.url,.sha256] | @tsv' "$spec")

java=usr/lib/jvm/java-21-openjdk

while IFS= read -r -d '' file; do
	[[ -f $file ]] ||
		continue
	resolved=$(realpath "$file")
	[[ $resolved == "$work/source/"* ]] ||
		continue
	copy "$file" "${file#"$work/source/"}"
done < <(find "$work/source/$java" \( -type f -o -type l \) -print0)

for file in libz.so.1 libgcc_s.so.1 libstdc++.so.6; do
	copy "$work/source/usr/lib/$file" "$java/lib/$file"
done

# Записать стандартный JKS с постоянными датами без приватных ключей
mkdir -p "$payload/$java/lib/security"
java "$ROOT/build/TrustStore.java" "$SDK/apps/apps/common/tls/ca-certificates.crt" \
	"$payload/$java/lib/security/cacerts"
"$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none '-DJAVA_HOME="/usr/lib/jvm/java-21-openjdk"' \
	"$port/launch.c" -o "$payload/bin/java"
copy "$port/help.md" usr/share/doc/openjdk/README.md
