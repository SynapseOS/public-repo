# recipe.sh: упаковать Oh My Zsh для SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: установить проверенные скрипты без изменения домашнего каталога пользователя
# Подсистема: сборка пакетов
# Первое изменение: 30.10.2026
# Последнее изменение: 30.10.2026
# Доступ: общедоступный

prepare

destination=usr/share/oh-my-zsh
for directory in lib plugins themes functions completions tools custom; do
	if [[ ! -d $source/$directory ]]; then
		continue
	fi

	while IFS= read -r -d '' file; do
		relative=${file#"$source/"}
		copy "$file" "$destination/$relative"
	done < <(find "$source/$directory" \( -type f -o -type l \) -print0 | sort -z)
done

copy "$source/oh-my-zsh.sh" "$destination/oh-my-zsh.sh"
copy "$source/templates/zshrc.zsh-template" "$destination/zshrc.zsh-template"
copy "$source/LICENSE.txt" usr/share/licenses/oh-my-zsh/LICENSE.txt
