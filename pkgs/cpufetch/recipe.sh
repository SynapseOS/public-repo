# recipe.sh: собрать cpufetch из закреплённого исходника
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 30.10.2026, последнее изменение: 30.10.2026
# Статус доступа: общедоступный

prepare

make -C "$source" -j"$JOBS" CC="$CC" os=SynapseOS GIT_VERSION=v1.07 \
    CFLAGS='-O2 -std=c99 -DARCH_X86 -D__synapse__ -static -pthread' cpufetch

copy "$source/cpufetch" bin/cpufetch
copy "$source/LICENSE" usr/share/licenses/cpufetch/LICENSE
