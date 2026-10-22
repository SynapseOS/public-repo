# recipe.sh: Собрать нативные компиляторы C и C++ без приватного дерева ядра
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

prepare
source "$ROOT/build/toolchain.sh"
export ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnp=no ac_cv_func_vfork_works=no
rm -rf "$work/deps" "$work/build"
mkdir -p "$work/deps" "$work/build"

for archive in gmp-6.2.1.tar.bz2 mpfr-4.1.0.tar.bz2 mpc-1.2.1.tar.gz; do
	sum=$(awk -v file="$archive" '$2==file {print $1}' "$source/contrib/prerequisites.sha512")
	[[ $sum =~ ^[0-9a-f]{128}$ ]]

	if [[ ! -f $work/$archive ]]; then
		curl --fail --location --proto '=https' --proto-redir '=https' \
			"https://gcc.gnu.org/pub/gcc/infrastructure/$archive" -o "$work/$archive.download"
		printf '%s  %s\n' "$sum" "$work/$archive.download" | sha512sum --check --status
		mv "$work/$archive.download" "$work/$archive"
	fi

	printf '%s  %s\n' "$sum" "$work/$archive" | sha512sum --check --status
	bsdtar -xf "$work/$archive" -C "$work/deps"
	component=${archive%%-*}
	directory=$(find "$work/deps" -maxdepth 1 -type d -name "$component-[0-9]*" | head -1)
	mkdir -p "$work/deps/$component-build"
	arguments=(--build=x86_64-pc-linux-gnu --host=x86_64-linux-musl "--prefix=$work/deps/install"
		--disable-shared --enable-static)

	case "$component" in
		gmp)
			arguments+=(--disable-assembly --disable-cxx)
			;;
		mpfr)
			arguments+=("--with-gmp=$work/deps/install" --disable-thread-safe)
			;;
		mpc)
			arguments+=("--with-gmp=$work/deps/install" "--with-mpfr=$work/deps/install")
			;;
	esac

	(
		cd "$work/deps/$component-build" &&
			"$directory/configure" "${arguments[@]}"
	)

	make -C "$work/deps/$component-build" -j"$JOBS"
	make -C "$work/deps/$component-build" install
done

(
	cd "$work/build" &&
		"$source/configure" --build=x86_64-pc-linux-gnu --host=x86_64-linux-musl \
			--target=x86_64-linux-musl --prefix=/usr --libexecdir=/usr/libexec --enable-languages=c,c++ \
			--disable-bootstrap --disable-multilib --disable-nls --disable-shared --disable-threads \
			--disable-libsanitizer --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic \
			--disable-libstdcxx --disable-lto --disable-plugin --disable-werror --without-isl \
			--with-gmp="$work/deps/install" --with-mpfr="$work/deps/install" --with-mpc="$work/deps/install" \
			--with-native-system-header-dir=/usr/include --with-as=/bin/as --with-ld=/bin/ld --with-pkgversion=SynapseOS
)

make -C "$work/build" -j"$JOBS" all-gcc
target=x86_64-linux-musl/14.3.0
copy "$work/build/gcc/xgcc" "usr/libexec/gcc/$target/gcc"
copy "$work/build/gcc/xg++" "usr/libexec/gcc/$target/g++"
copy "$work/build/gcc/cc1" "usr/libexec/gcc/$target/cc1"
copy "$work/build/gcc/cc1plus" "usr/libexec/gcc/$target/cc1plus"
"$CC" -O2 -Wall -Wextra -Werror "$port/gcc-wrapper.c" -o "$payload/bin/gcc"
"$CC" -O2 -Wall -Wextra -Werror "$port/gxx-wrapper.c" -o "$payload/bin/g++"
copy "$payload/bin/gcc" bin/cc
copy "$port/specs" "usr/lib/gcc/$target/specs"

while IFS= read -r -d '' file; do
	copy "$file" "usr/lib/gcc/$target/include/${file#"$work/build/gcc/include/"}"
done < <(find "$work/build/gcc/include" -type f -print0)

make -C "$SDK/apps/musl" install-headers DESTDIR="$work/headers" includedir=/usr/include

while IFS= read -r -d '' file; do
	copy "$file" "usr/include/${file#"$work/headers/usr/include/"}"
done \
	< <(find "$work/headers/usr/include" -type f -print0)

for file in libc.a libm.a libpthread.a librt.a libdl.a libutil.a libresolv.a crt1.o crti.o crtn.o; do
	copy "$SDK/apps/musl/lib/$file" "usr/lib/$file"
done

for file in libgcc.a libgcc_eh.a crtbeginT.o crtend.o; do
	copy "$(gcc -print-file-name="$file")" "usr/lib/gcc/$target/$file"
done

copy "$BUILD_ROOT/libstdcxx/sysroot/lib/libsynapse-cxx.a" "usr/lib/gcc/$target/libsynapse-cxx.a"
copy "$port/synapse.ld" "usr/lib/gcc/$target/synapse.ld"

for file in COPYING3 COPYING.RUNTIME; do
	copy "$source/$file" "usr/share/licenses/gcc/$file"
done

find "$payload/usr/libexec" -type f -exec strip --strip-debug {} +
