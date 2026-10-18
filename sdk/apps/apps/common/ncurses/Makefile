CC := $(abspath ../musl-cc)
PREFIX := $(CURDIR)/install
.PHONY: all clean
all: install/lib/libncursesw.a
.build/Makefile: upstream/configure Makefile
	mkdir -p .build
	cd .build && ../upstream/configure --host=x86_64-linux-musl --build=x86_64-linux-gnu CC='$(CC)' AR=ar RANLIB=ranlib --prefix='$(PREFIX)' --enable-widec --without-shared --without-debug --without-ada --without-cxx --without-cxx-binding --without-progs --without-tests --disable-db-install --with-fallbacks=linux,xterm,xterm-256color,vt100,ansi --without-manpages
install/lib/libncursesw.a: .build/Makefile
	$(MAKE) -C .build -j4
	$(MAKE) -C .build install.libs install.includes
clean:
	rm -rf .build install
