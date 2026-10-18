# recipe.sh: Собрать нативную библиотеку из открытого SDK
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

make -C "$SDK/apps/synlibc" -j"$JOBS"

for file in libsyn.a synlibc.a; do
	copy "$SDK/apps/synlibc/$file" "usr/lib/$file"
done

for file in orbit.h syn.h inference.h; do
	copy "$SDK/include/synapse/$file" "usr/include/synapse/$file"
done

while IFS= read -r -d '' file; do
	rel=${file#"$SDK/apps/synlibc/include/"}

	if [[ $rel != synapse/* ]]; then
		copy "$file" "usr/include/synlibc/$rel"
	fi
done < <(find "$SDK/apps/synlibc/include" -name '*.h' -type f -print0)
