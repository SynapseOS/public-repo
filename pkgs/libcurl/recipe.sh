# recipe.sh: собрать статическую libcurl с TLS для OSTree
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Описание: библиотека HTTP-клиента из закреплённого исходника curl
# Подсистема: сборка пакетов
# Первое изменение: 25.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный

prepare

openssl="$BUILD_ROOT/openssl/payload/usr"
[[ -f $openssl/lib/libssl.a ]]
[[ -f $openssl/lib/libcrypto.a ]]

(
	cd "$source"
	CC="$CC" CFLAGS='-O2 -Werror -fPIC -pthread' \
		CPPFLAGS="-D_GNU_SOURCE -D__synapse__ -I$openssl/include" \
		LDFLAGS="-L$openssl/lib -pthread" \
		ac_cv_func_getifaddrs=no ac_cv_func_eventfd=no \
		./configure --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --disable-shared --enable-static --disable-threaded-resolver \
			--disable-ares --disable-ipv6 --disable-ldap --disable-ldaps \
			--disable-rtsp --disable-dict --disable-telnet --disable-tftp \
			--disable-smb --disable-pop3 --disable-imap --disable-smtp \
			--disable-gopher --disable-mqtt --disable-ftp \
			--without-libpsl --without-libidn2 --without-librtmp \
			--without-nghttp2 --without-nghttp3 --without-ngtcp2 \
			--without-libssh2 --without-zstd --without-brotli --without-zlib \
			--with-openssl="$openssl" --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
			--without-ca-path

	make -j"$JOBS" -C lib
)

copy "$source/lib/.libs/libcurl.a" usr/lib/libcurl.a

for header in "$source/include/curl/"*.h; do
	copy "$header" "usr/include/curl/$(basename "$header")"
done

copy "$source/COPYING" usr/share/licenses/libcurl/COPYING

mkdir -p "$payload/usr/lib/pkgconfig"
cat >"$payload/usr/lib/pkgconfig/libcurl.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: libcurl
Description: HTTP client library
Version: 8.22.0
Libs: -L${libdir} -lcurl -lssl -lcrypto -pthread
Cflags: -I${includedir}
PC
