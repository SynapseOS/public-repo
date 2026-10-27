# recipe.sh: собрать GnuPG для штатной проверки подписей Flatpak
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 26.10.2026, последнее изменение: 26.10.2026
# Статус доступа: общедоступный

prepare

sysroot=$(mktemp -d /tmp/synapse-gnupg-sysroot.XXXXXX)
trap 'rm -rf -- "$sysroot"' EXIT
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib" "$sysroot/usr/bin"

for package in libgpg-error libassuan libgcrypt libksba npth zlib; do
	stage="$BUILD_ROOT/$package/payload"
	if [[ -d $stage/usr/include ]]; then
		cp -a "$stage/usr/include/." "$sysroot/usr/include/"
	fi

	if [[ -d $stage/usr/lib ]]; then
		cp -a "$stage/usr/lib/." "$sysroot/usr/lib/"
	fi
done

for config in gpg-error-config libassuan-config libgcrypt-config ksba-config npth-config; do
	package=libgpg-error
	case "$config" in
		libassuan-config) package=libassuan ;;
		libgcrypt-config) package=libgcrypt ;;
		ksba-config) package=libksba ;;
		npth-config) package=npth ;;
	esac

	sed -e "s|^prefix=/usr$|prefix=$sysroot/usr|" \
		-e "s|^libdir=/usr/lib$|libdir=$sysroot/usr/lib|" \
		"$BUILD_ROOT/$package/payload/bin/$config" >"$sysroot/usr/bin/$config"
	chmod +x "$sysroot/usr/bin/$config"
done

mkdir -p "$work/build"
(
	cd "$work/build"
	PATH="$sysroot/usr/bin:$PATH" CC="$CC" \
		CFLAGS="-O2 -I$sysroot/usr/include" \
		LDFLAGS="-static -L$sysroot/usr/lib" \
		"$source/configure" --build=x86_64-linux-gnu --host=x86_64-linux-musl \
			--prefix=/usr --libdir=/usr/lib --sysconfdir=/etc --localstatedir=/var \
			--with-libgpg-error-prefix="$sysroot/usr" \
			--with-libgcrypt-prefix="$sysroot/usr" \
			--with-libassuan-prefix="$sysroot/usr" \
			--with-libksba-prefix="$sysroot/usr" \
			--with-npth-prefix="$sysroot/usr" \
			--disable-nls --disable-doc --disable-gpgsm --disable-scdaemon \
			--disable-dirmngr --disable-keyboxd --disable-wks-tools --disable-ldap
	make -j"$JOBS"
)

copy "$work/build/g10/gpg" usr/bin/gpg
copy "$work/build/g10/gpgv" usr/bin/gpgv
copy "$work/build/agent/gpg-agent" usr/bin/gpg-agent
copy "$work/build/tools/gpgconf" usr/bin/gpgconf
copy "$source/COPYING" usr/share/licenses/gnupg/COPYING
