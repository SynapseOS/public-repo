# recipe.sh: Сборка Redis с musl
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: серверные пакеты, первое изменение: 13.11.2026, последнее изменение: 23.11.2026
# Статус доступа: общедоступный

prepare
# Redis выделяет блок сообщения по длине пакета, GCC оценивает полную структуру
make -C "$source/src" -j"$JOBS" CC="$CC" MALLOC=libc USE_SYSTEMD=no BUILD_TLS=no \
    OPT=-O2 CFLAGS='-Werror -Wno-array-bounds' REDIS_LDFLAGS=-static TARGET_ARCH= redis-server redis-cli

copy "$source/src/redis-server" bin/redis-server
copy "$source/src/redis-cli" bin/redis-cli
copy "$source/LICENSE.txt" usr/share/licenses/redis/LICENSE
copy "$port/redis.conf" etc/redis/redis.conf
copy "$port/service" etc/services/redis
