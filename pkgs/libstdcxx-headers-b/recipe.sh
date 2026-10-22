# recipe.sh: Упаковать вторую часть заголовков C++ для SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Файл: recipe.sh, подсистема: сборка пакетов
# Первое изменение: 22.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

source_work="$BUILD_ROOT/libstdcxx"
mapfile -d '' headers < <(find "$source_work/sysroot/include/c++" -type f -print0 | sort -z)
[[ ${#headers[@]} -gt 0 && ${#headers[@]} -le 1000 ]]
half=$(( (${#headers[@]} + 1) / 2 ))

for file in "${headers[@]:half}"; do
	copy "$file" "usr/${file#"$source_work/sysroot/"}"
done

for file in COPYING3 COPYING.RUNTIME; do
	copy "$source_work/gcc-14.3.0/$file" "usr/share/licenses/$name/$file"
done
