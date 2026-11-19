# recipe.sh: Упаковать Node.js 22 LTS для musl
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: серверные пакеты, первое и последнее изменение: 13.11.2026
# Статус доступа: общедоступный

fetch "$(jq -er .source "$spec")" "$(jq -er .sha256 "$spec")" "$work/source.tar"
source_directory=$(jq -er .source_directory "$spec")
tar -xJf "$work/source.tar" -C "$work" "$source_directory/bin/node" "$source_directory/LICENSE" \
    "$source_directory/lib/node_modules/npm"
source="$work/$source_directory"
runtime=https://dl-cdn.alpinelinux.org/alpine/v3.24/main/x86_64
fetch "$runtime/libgcc-15.2.0-r5.apk" \
    393dcd32629f06d7d85409c272d142d0c082772d10b87ef55ee82f47de3be637 "$work/libgcc.apk"
fetch "$runtime/libstdc++-15.2.0-r5.apk" \
    14c987b556f5385a5db18376e788c75f37d85321b8dc1920d926ea7daac1d6f6 "$work/libstdcxx.apk"

mkdir -p "$work/gcc-runtime"
tar --warning=no-unknown-keyword -xzf "$work/libgcc.apk" \
    -C "$work/gcc-runtime" usr/lib/libgcc_s.so.1
tar --warning=no-unknown-keyword -xzf "$work/libstdcxx.apk" \
    -C "$work/gcc-runtime" usr/lib/libstdc++.so.6.0.34

copy "$work/gcc-runtime/usr/lib/libgcc_s.so.1" usr/lib/nodejs22/libgcc_s.so.1
copy "$work/gcc-runtime/usr/lib/libstdc++.so.6.0.34" usr/lib/nodejs22/libstdc++.so.6
copy "$source/bin/node" bin/node22
copy "$source/LICENSE" usr/share/licenses/nodejs22/LICENSE
mkdir -p "$payload/usr/lib/nodejs22/node_modules"
cp -a "$source/lib/node_modules/npm" "$payload/usr/lib/nodejs22/node_modules/npm"
cat > "$payload/bin/npm22" <<'EOF'
#!/bin/sh
exec /bin/node22 /usr/lib/nodejs22/node_modules/npm/bin/npm-cli.js "$@"
EOF
cat > "$payload/bin/npx22" <<'EOF'
#!/bin/sh
exec /bin/node22 /usr/lib/nodejs22/node_modules/npm/bin/npx-cli.js "$@"
EOF
chmod 755 "$payload/bin/npm22" "$payload/bin/npx22"
strip --strip-debug "$payload/bin/node22"
patchelf --force-rpath --set-rpath /usr/lib/nodejs22 "$payload/bin/node22"
