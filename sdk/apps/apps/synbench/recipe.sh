# recipe.sh: Собрать новый synbench по формату public-repo
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
# Подсистема: сборка пользовательских пакетов
# Первое изменение: 19.10.2026, последнее изменение: 19.10.2026
# Доступ: общедоступный

source "$ROOT/build/toolchain.sh"
flags=(-O2 -std=c11 -march=x86-64 -Wall -Wextra -Werror -Wconversion -Wshadow -Wl,--build-id=none)
"$CC" "${flags[@]}" "$port/guest.c" -o "$payload/bin/synbench"
"$CC" "${flags[@]}" -fno-builtin-memcpy "$port/workload.c" -o "$payload/bin/synbench-workload"
"$CC" "${flags[@]}" \
    -DQWEN_SOURCE="\"$SDK/apps/apps/qwen3.c/runq.c\"" "$port/qwen.c" -lm -o "$payload/bin/synbench-qwen"
copy "$port/host.py" usr/share/synbench/host.py
copy "$port/metrics.py" usr/share/synbench/metrics.py
copy "$port/recording.py" usr/share/synbench/recording.py
copy "$SDK/apps/apps/qwen3.c/LICENSE" usr/share/licenses/synbench/qwen3-c-LICENSE
