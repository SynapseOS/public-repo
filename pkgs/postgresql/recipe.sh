# recipe.sh: Сборка PostgreSQL 16 с musl
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: серверные пакеты, первое изменение: 13.11.2026, последнее изменение: 23.11.2026
# Статус доступа: общедоступный

prepare
prefix=/usr/lib/postgresql/16
(
    cd "$source"
    ./configure --prefix="$prefix" --without-icu --without-readline --without-zlib CC="${POSTGRES_MUSL_CC:-musl-gcc}"
)
env -u TARGET_ARCH make -C "$source/src/backend" -j"$JOBS" generated-headers
env -u TARGET_ARCH make -C "$source" -j"$JOBS"
staged="$work/install"
rm -rf -- "$staged"
env -u TARGET_ARCH make -C "$source" DESTDIR="$staged" install
for binary in postgres initdb pg_ctl pg_isready psql; do
    copy "$staged$prefix/bin/$binary" "$prefix/bin/$binary"
done
while IFS= read -r -d '' file; do
    copy "$file" "${file#"$staged"}"
done < <(find "$staged$prefix/share" -type f -print0)
for library in "$staged$prefix/lib/"*.so "$staged$prefix/lib/libpq.so.5"; do
    [[ -L $library && $library != */libpq.so.5 ]] && continue
    copy "$library" "$prefix/lib/${library##*/}"
done
copy "$source/COPYRIGHT" usr/share/licenses/postgresql/COPYRIGHT
copy "$port/service" etc/services/postgres
mkdir -p "$payload/usr/libexec"
"${POSTGRES_MUSL_CC:-musl-gcc}" -ffreestanding -Wall -Wextra -Werror -static "$port/postgres-start.c" -o "$payload/usr/libexec/postgres-start"
