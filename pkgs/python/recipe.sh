# recipe.sh: Собрать CPython с нативными встроенными модулями и отдельной потоковой OpenSSL
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
headers=$(gcc -print-file-name=include)
fetch "$(jq -r .openssl.source "$spec")" "$(jq -r .openssl.sha256 "$spec")" "$work/openssl.tar.gz"
bsdtar -xf "$work/openssl.tar.gz" -C "$work"
tls_source="$work/$(jq -r .openssl.source_directory "$spec")"
openssl="$work/openssl-install"
sed -i 's|#define X509_CERT_FILE OPENSSLDIR "/cert.pem"|#define X509_CERT_FILE "/etc/ssl/certs/ca-certificates.crt"|' \
	"$tls_source/include/internal/common.h"

(
	cd "$tls_source" &&
		CFLAGS="-O2 -D__synapse__ -isystem $headers" LDFLAGS='' ./Configure linux-x86_64 \
			--prefix="$openssl" --libdir=lib --openssldir=/etc/ssl no-shared no-async no-asm no-dso no-engine \
			no-tests no-module no-secure-memory -D__synapse__ -isystem "$headers"
)

make -C "$tls_source" -j"$JOBS" build_libs
make -C "$tls_source" install_dev
host_python=$(command -v python3.13 ||
	true)

if [[ -z $host_python ]]; then
	mkdir -p "$work/host-build"

	(
		cd "$work/host-build" &&
			env CC=gcc CFLAGS=-O2 LDFLAGS='' "$source/configure" \
				--without-ensurepip --disable-test-modules --prefix="$work/host-install"
	)

	env CC=gcc make -C "$work/host-build" -j"$JOBS"
	host_python="$work/host-build/python"
fi

zlib="$BUILD_ROOT/zlib/payload/usr"
sqlite="$BUILD_ROOT/sqlite/payload/usr"
export CC="$ROOT/pkgs/python/cc" CFLAGS='-O2 -D_GNU_SOURCE -D__synapse__ -ffunction-sections -fdata-sections'
export CPPFLAGS="-I$zlib/include" LDFLAGS="-static -Wl,--gc-sections -L$zlib/lib"
export ZLIB_CFLAGS="-I$zlib/include" ZLIB_LIBS="$zlib/lib/libz.a"
export LIBSQLITE3_CFLAGS="-I$sqlite/include" LIBSQLITE3_LIBS="-L$sqlite/lib -lsqlite3 -lm"
export PKG_CONFIG=false MODULE_BUILDTYPE=static
export ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no

for function in getentropy memfd_create eventfd epoll_create1 epoll_create pidfd_open pidfd_getfd \
	vfork posix_spawn posix_spawnp; do
	export "ac_cv_func_$function=no"
done

(
	cd "$source" &&
		./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
			--disable-shared --without-ensurepip --disable-test-modules --with-build-python="$host_python" \
			--with-openssl="$openssl" --with-openssl-rpath=no --without-readline
)

grep -q 'MODULE__SQLITE3_STATE=yes' "$source/config.log"
make -C "$source" -j"$JOBS" python pybuilddir.txt
copy "$source/python" bin/python3
strip --strip-debug "$payload/bin/python3"
copy "$port/pip.conf" etc/pip.conf
wheel="$source/$(jq -r .pip_bootstrap.archive_member "$spec")"
printf '%s  %s\n' "$(jq -r .pip_bootstrap.sha256 "$spec")" "$wheel" | sha256sum --check --status
mkdir -p "$work/stdlib" "$payload/usr/lib/python3.13/site-packages"

while IFS= read -r -d '' file; do
	relative=${file#"$source/Lib/"}

	case "/$relative" in
		*/ensurepip/* | */test/* | */tests/* | */__pycache__/* | */idlelib/* | */tkinter/* | *.pyc | *.pyo)
			continue
			;;
	esac

	mkdir -p "$work/stdlib/$(dirname "$relative")"
	cp "$file" "$work/stdlib/$relative"
done < <(find "$source/Lib" -type f -print0)

find "$work/stdlib" -type f -exec touch -t 198001010000 {} +

(
	cd "$work/stdlib" &&
		find . -type f -printf '%P\n' | sort | zip -X -q -9 "$payload/usr/lib/python313.zip" -@
)

bsdtar -xf "$wheel" -C "$payload/usr/lib/python3.13/site-packages"

while IFS= read -r -d '' file; do
	copy "$file" "usr/lib/python3.13/ensurepip/${file#"$source/Lib/ensurepip/"}"
done < <(find "$source/Lib/ensurepip" -type f \( -name '*.py' -o -name "$(basename "$wheel")" \) -print0)

"$SDK/apps/apps/common/musl-cc" -O2 "$port/pip-main.c" -o "$work/pip-launcher"

for file in pip pip3 pip3.13; do
	copy "$work/pip-launcher" "bin/$file"
done

copy "$source/Lib/os.py" usr/lib/python3.13/os.py
mkdir -p "$payload/usr/lib/python3.13/lib-dynload"
printf 'SynapseOS uses static builtin extensions\n' >"$payload/usr/lib/python3.13/lib-dynload/README"

for file in "$source/$(cat "$source/pybuilddir.txt")/"_sysconfigdata_*.py; do
	copy "$file" "usr/lib/python3.13/$(basename "$file")"
done

copy "$source/LICENSE" usr/share/licenses/python/LICENSE

for dependency in openssl zlib; do
	licenses="$BUILD_ROOT/$dependency/payload/usr/share/licenses/$dependency"

	while IFS= read -r -d '' file; do
		copy "$file" "usr/share/licenses/python/$dependency/${file#"$licenses/"}"
	done < <(find "$licenses" -type f -print0)
done
