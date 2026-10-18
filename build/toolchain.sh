# toolchain.sh: Подготовить компиляторы со схемой ELF и TLS для нативных инструментов
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

sed -e '/^base=/,/^musl=/{ /^musl=/!d; }' \
	-e "/^musl=/i base='$ROOT/apps/apps'" \
	-e "s|\$base/common/apps.ld|$ROOT/pkgs/gcc/synapse.ld|g" \
	"$SDK/apps/apps/common/musl-cc" >"$work/musl-cc"

sed -e '/^root=/,/^musl=/{ /^musl=/!d; }' \
	-e "/^musl=/i root='$ROOT'" \
	-e 's|pkgs/libstdcxx/cxx.ld|pkgs/gcc/synapse.ld|g' \
	"$ROOT/pkgs/libstdcxx/cxx" >"$work/musl-cxx"

for compiler in "$work/musl-cc" "$work/musl-cxx"; do
	sh -n "$compiler"
done

chmod 755 "$work/musl-cc" "$work/musl-cxx"
export CC="$work/musl-cc" CXX="$work/musl-cxx" AR=ar RANLIB=ranlib
export CFLAGS='-O2 -D_GNU_SOURCE' CXXFLAGS='-O2 -D_GNU_SOURCE' LDFLAGS=-static
