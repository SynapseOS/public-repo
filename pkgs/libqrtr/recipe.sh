# recipe.sh: Закреплённая BSD libqrtr и локальная ABI проба
# Автор: Елчинян Арен Артурович 2026
# Архитектура: AArch64 и x86_64, ABI: musl и Linux AF_QIPCRTR
# Описание: Upstream исходники не изменяются, Android и Linux kernel не подменяют ОС
# Подсистема: публичные пакеты
# Первое изменение: 25.11.2026
# Последнее изменение: 25.11.2026
# Доступ: общедоступный
prepare
if [[ -n ${QRTR_CC:-} ]]; then
    CC=$QRTR_CC
fi
flags=(-O2 -D_GNU_SOURCE -ffunction-sections -fdata-sections -I"$port/compat" -I"$source/include")
objects=()
for unit in logging qmi qrtr; do
    "$CC" "${flags[@]}" -c "$source/lib/$unit.c" -o "$work/$unit.o"
    objects+=("$work/$unit.o")
done
mkdir -p "$payload/usr/lib" "$payload/usr/lib/pkgconfig"
"${QRTR_AR:-ar}" rcs "$payload/usr/lib/libqrtr.a" "${objects[@]}"
"$CC" -O2 -Wall -Wextra -Werror -I"$port/compat" -I"$source/include" \
    "$port/qrtr-probe.c" "$payload/usr/lib/libqrtr.a" -static -Wl,--gc-sections -o "$payload/bin/qrtr-probe"
copy "$source/include/libqrtr.h" usr/include/libqrtr.h
copy "$port/compat/linux/qrtr.h" usr/include/linux/qrtr.h
copy "$source/LICENSE" usr/share/licenses/libqrtr/LICENSE
cat > "$payload/usr/lib/pkgconfig/qrtr.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: qrtr
Description: BSD userspace Qualcomm IPC Router library
Version: 1.2
Libs: -L${libdir} -lqrtr
Cflags: -I${includedir}
PC
