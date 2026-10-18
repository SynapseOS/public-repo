# recipe.sh: Собрать проверки Orbit и сравнение с POSIX
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

source "$ROOT/build/toolchain.sh"
"$CC" -O2 -std=gnu11 -Wall -Wextra -Werror -I"$SDK/include" "$port/main.c" \
	"$SDK/apps/synlibc/libsyn.a" -pthread -o "$payload/bin/synbench"

for variant in posix synapi synapi-reference posix-fast; do
	flags=()

	case "$variant" in
		synapi | synapi-reference)
			flags+=(-DQWEN_ORBIT)
			;;
	esac

	case "$variant" in
		synapi | posix-fast)
			flags+=(-DQWEN_FAST)
			;;
	esac

	case "$variant" in
		synapi | synapi-reference | posix-fast)
			flags+=("$SDK/apps/synlibc/libsyn.a")
			;;
	esac

	"$CC" -O3 -march=x86-64-v3 -ffunction-sections -fdata-sections -I"$SDK/include" \
		"$SDK/tests/qwen3-orbit-bench.c" "${flags[@]}" -lm -o "$payload/bin/qwen3-$variant-bench"
done

copy "$SDK/apps/apps/qwen3.c/LICENSE" usr/share/licenses/synbench/qwen3-c-LICENSE
