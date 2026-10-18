# recipe.sh: Собрать Git с HTTPS и без рабочих потоков
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
ssl="$BUILD_ROOT/openssl/payload/usr"
zlib="$BUILD_ROOT/zlib/payload/usr"
curl="$BUILD_ROOT/curl/curl-8.22.0"
arguments=("CC=$CC" 'CFLAGS=-O2 -D_GNU_SOURCE -D__synapse__ -ffunction-sections -fdata-sections'
	'LDFLAGS=-static -Wl,--gc-sections' prefix=/usr gitexecdir=/usr/libexec/git-core "ZLIB_PATH=$zlib"
	"CURL_CFLAGS=-I$curl/include" "CURL_LIBCURL=$curl/lib/.libs/libcurl.a $ssl/lib/libssl.a $ssl/lib/libcrypto.a"
	SHELL_PATH=/bin/bash)

for option in NO_RUST NO_GETTEXT NO_PTHREADS NO_PERL NO_PYTHON NO_TCLTK NO_EXPAT NO_OPENSSL NO_REGEX \
	NO_D_TYPE_IN_DIRENT NO_LINUX_PROCFS NO_UNIX_SOCKETS NO_MMAP; do
	arguments+=("$option=YesPlease")
done

make -C "$source" -j"$JOBS" "${arguments[@]}" git git-remote-http
copy "$source/git" bin/git

for file in git-upload-pack git-receive-pack; do
	copy "$source/git" "usr/libexec/git-core/$file"
done

for file in git-remote-http git-remote-https; do
	copy "$source/git-remote-http" "usr/libexec/git-core/$file"
done

while IFS= read -r -d '' file; do
	case $(basename "$file") in
		Makefile | meson.build)
			continue
			;;
	esac

	copy "$file" "usr/share/git-core/templates/${file#"$source/templates/"}"
done < <(find "$source/templates" -type f -print0)

copy "$source/COPYING" usr/share/licenses/git/COPYING

for dependency in curl openssl zlib; do
	licenses="$BUILD_ROOT/$dependency/payload/usr/share/licenses/$dependency"

	while IFS= read -r -d '' file; do
		copy "$file" "usr/share/licenses/git/$dependency/${file#"$licenses/"}"
	done < <(find "$licenses" -type f -print0)
done
