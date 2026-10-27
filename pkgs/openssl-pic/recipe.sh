# recipe.sh: собрать OpenSSL с позиционно-независимым кодом
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: статические библиотеки для PIE-портов без замены системного OpenSSL
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare
export SYNAPSE_LINK_MODE=dynamic

(
	cd "$source"
	CC="$CC" ./Configure linux-x86_64 --prefix=/usr --openssldir=/etc/ssl \
		no-shared no-threads no-async no-dso no-engine no-tests no-module \
		no-secure-memory -D__synapse__ -fPIC -isystem "$(gcc -print-file-name=include)"

	make -j"$JOBS" build_libs
)

copy "$source/libssl.a" usr/lib/pic/libssl.a
copy "$source/libcrypto.a" usr/lib/pic/libcrypto.a

for header in "$source/include/openssl/"*.h; do
	copy "$header" "usr/include/openssl/$(basename "$header")"
done

copy "$source/LICENSE.txt" usr/share/licenses/openssl-pic/LICENSE.txt
