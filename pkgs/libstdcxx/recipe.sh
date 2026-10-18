# recipe.sh: Собрать C++ runtime для целевой libc без заголовков libc хоста
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

prepare
headers=$(gcc -print-file-name=include)
sed -e '/^base=/,/^musl=/{ /^musl=/!d; }' \
	-e "/^musl=/i base='$ROOT/apps/apps'" -e 's/exec gcc/exec g++/g' \
	-e "s|-nostdinc |-nostdinc -nostdinc++ -isystem $headers |g" \
	"$SDK/apps/apps/common/musl-cc" >"$work/bootstrap-cxx"
sh -n "$work/bootstrap-cxx"
chmod 755 "$work/bootstrap-cxx"
export CXX="$work/bootstrap-cxx" CFLAGS="-O2 -D_GNU_SOURCE -D__synapse__ -isystem $headers"
export CXXFLAGS='-O2 -D_GNU_SOURCE -D__synapse__' LDFLAGS=-static
rm -rf "$work/build" "$work/sysroot"
mkdir -p "$work/build"

(
	cd "$work/build" &&
		"$source/libstdc++-v3/configure" --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
			--prefix="$work/sysroot" --disable-multilib --disable-shared --enable-static --disable-nls \
			--disable-libstdcxx-pch --disable-libstdcxx-threads --enable-clocale=generic --without-libstdcxx-zoneinfo
)

make -C "$work/build" -j"$JOBS"
make -C "$work/build" install
"$CC" -O2 -c "$port/dl-find-object.c" -o "$work/dl-find-object.o"
ar rcs "$work/sysroot/lib/libsynapse-cxx.a" "$work/dl-find-object.o"

for file in libstdc++.a libsupc++.a libstdc++exp.a libstdc++fs.a libsynapse-cxx.a; do
	copy "$work/sysroot/lib/$file" "usr/lib/$file"
done

mkdir -p "$payload/usr/share/libstdcxx"
tar --mtime=@0 --owner=0 --group=0 --numeric-owner -C "$work/sysroot" -cf - include | gzip -n \
	>"$payload/usr/share/libstdcxx/headers.tar.gz"

for file in COPYING3 COPYING.RUNTIME; do
	copy "$source/$file" "usr/share/licenses/libstdcxx/$file"
done
