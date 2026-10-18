# recipe.sh: Собрать сканер и разнести данные NSE по пакетам с ограничением числа файлов
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
cp "$port/patches/dnet-synapse.c" "$source/libdnet-stripped/src/eth-none.c"

for file in intf.c route-none.c; do
	printf '// Реализация находится в eth-none.c\n' >"$source/libdnet-stripped/src/$file"
done

sed -i -e '/as_fn_error.*Ethernet support not found for this system/d' \
	-e '/as_fn_error.*Routing interface not found for this system/d' \
	"$source/libdnet-stripped/configure"
export CXX="$ROOT/pkgs/libstdcxx/cxx" PKG_CONFIG=false ax_cv_have_epoll=no
export CFLAGS="-O2 -D__synapse__ -I$SDK/include -I$BUILD_ROOT/zlib/payload/usr/include"
export CXXFLAGS="$CFLAGS" LDFLAGS="-static -L$BUILD_ROOT/zlib/payload/usr/lib"

(
	cd "$source" &&
		./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
			--without-ncat --without-nping --without-zenmap --without-ndiff --without-openssl --without-libssh2 \
			--with-libpcap="$BUILD_ROOT/libpcap/payload/usr" --with-libpcre=included --with-liblua=included \
			--with-libz="$BUILD_ROOT/zlib/payload/usr"
)

make -C "$source" -j"$JOBS"
copy "$source/nmap" bin/nmap
strip --strip-debug "$payload/bin/nmap"

for file in nmap-services nmap-rpc nmap-os-db nmap-service-probes nmap-protocols nmap-mac-prefixes nse_main.lua; do
	copy "$source/$file" "usr/share/nmap/$file"
done

for file in nmap.dtd nmap.xsl; do
	copy "$source/docs/$file" "usr/share/nmap/$file"
done

copy "$source/docs/nmap.1" usr/share/man/man1/nmap.1
copy "$source/LICENSE" usr/share/licenses/nmap/COPYING
parent_payload=$payload

for child in nmap-nse-a nmap-nse-b nmap-nselib; do
	payload="$BUILD_ROOT/$child/payload"
	rm -rf "$payload"
	mkdir -p "$payload"

	if [[ $child == nmap-nselib ]]; then
		mapfile -d '' files < <(find "$source/nselib" -type f -print0 | sort -z)
	else
		mapfile -d '' all < <(find "$source/scripts" -type f -print0 | sort -z)

		if [[ $child == nmap-nse-a ]]; then
			files=("${all[@]:0:350}")
		else
			files=("${all[@]:350}")
		fi
	fi

	for file in "${files[@]}"; do
		copy "$file" "usr/share/nmap/${file#"$source/"}"
	done

	copy "$source/LICENSE" "usr/share/licenses/$child/LICENSE"
	pack "$child" "$payload" "$ROOT/pkgs/$child/manifest.json"
done

payload=$parent_payload
