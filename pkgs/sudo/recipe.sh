# Собрать sudo и проверяющий синтаксис политики visudo со статической musl.
prepare
(
	cd "$source"
	export CFLAGS="-O2 -D__synapse__"
	export LDFLAGS="-static"
	./configure --build="$(gcc -dumpmachine)" --host="$TARGET_ARCH-linux-musl" \
		--prefix=/usr --bindir=/bin --sbindir=/bin --sysconfdir=/etc \
		--with-rundir=/run/sudo --with-vardir=/var/lib/sudo \
		--with-passwd --without-pam --without-sendmail --with-editor=/bin/vi \
		--disable-shared --enable-static-sudoers --disable-shared-libutil \
		--disable-log-server --disable-log-client --disable-openssl --disable-zlib --disable-nls
	grep -Fxq '#define HAVE_CRYPT 1' config.h
	grep -Fxq '#define HAVE_GETSPNAM 1' config.h
	make -j"$JOBS"
)
copy "$source/src/sudo" bin/sudo
copy "$source/plugins/sudoers/visudo" bin/visudo
copy "$port/sudoers" etc/sudoers
copy "$source/LICENSE.md" usr/share/licenses/sudo/LICENSE.md
chmod 4755 "$payload/bin/sudo"
chmod 0440 "$payload/etc/sudoers"
digest=$(sha256sum "$payload/bin/sudo" | cut -d ' ' -f1)
install -Dm644 /dev/null "$payload/usr/share/synapse/privileged-elf/$digest"
