# recipe.sh: Упаковать официальный Linux-бинарник Node.js 20
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: серверные пакеты, первое и последнее изменение: 13.11.2026
# Статус доступа: общедоступный

prepare
runtime="${NODEJS_GCC_RUNTIME:-/usr/lib/x86_64-linux-gnu}"
copy "$source/bin/node" usr/lib/nodejs/node
copy "$source/LICENSE" usr/share/licenses/nodejs/LICENSE
copy "$runtime/libstdc++.so.6" usr/lib/nodejs/libstdc++.so.6
copy "$runtime/libgcc_s.so.1" usr/lib/nodejs/libgcc_s.so.1
if [ -f "${NODEJS_GCC_COPYRIGHT:-/usr/share/doc/libstdc++6/copyright}" ]; then
    copy "${NODEJS_GCC_COPYRIGHT:-/usr/share/doc/libstdc++6/copyright}" usr/share/licenses/nodejs/GCC-copyright
fi
if [ -d "$source/lib/node_modules/npm" ]; then
    mkdir -p "$payload/usr/lib/nodejs/node_modules"
    cp -a "$source/lib/node_modules/npm" "$payload/usr/lib/nodejs/node_modules/npm"
    cat > "$payload/bin/npm" <<'EOF'
#!/bin/sh
exec /bin/node /usr/lib/nodejs/node_modules/npm/bin/npm-cli.js "$@"
EOF
    cat > "$payload/bin/npx" <<'EOF'
#!/bin/sh
exec /bin/node /usr/lib/nodejs/node_modules/npm/bin/npx-cli.js "$@"
EOF
    chmod 755 "$payload/bin/npm" "$payload/bin/npx"
fi
copy "$port/node" bin/node
chmod 755 "$payload/bin/node"
python3 "$port/interpreter.py" "$payload/usr/lib/nodejs/node"
