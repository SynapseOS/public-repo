# recipe.sh: BSD upstream RMTFS с private shadow backend
# Автор: Елчинян Арен Артурович 2026
# Архитектура: AArch64 и x86_64, ABI: musl, AF42 и scoped RMTFS ioctl
# Описание: Без libudev, sysfs, raw partition mode и физических устройств
# Подсистема: публичные пакеты телефона
# Первое изменение: 25.11.2026
# Последнее изменение: 25.11.2026
# Доступ: общедоступный
prepare
CC=${RMTFS_CC:-$CC}
qrtr=${RMTFS_QRTR_STAGE:?Set RMTFS_QRTR_STAGE to the matching libqrtr payload}
flags=(-O2 -D_GNU_SOURCE -DSYNAPSE_RMTFS=1 -ffunction-sections -fdata-sections
    -I"$port" -I"$source" -I"$qrtr/usr/include")
objects=()
for unit in qmi_rmtfs rmtfs sharedmem storage util; do
    "$CC" "${flags[@]}" -c "$source/$unit.c" -o "$work/$unit.o"
    objects+=("$work/$unit.o")
done
"$CC" "${flags[@]}" -c "$port/synapse-rproc.c" -o "$work/rproc.o"
"$CC" -static -Wl,--gc-sections "${objects[@]}" "$work/rproc.o" \
    "$qrtr/usr/lib/libqrtr.a" -o "$payload/bin/rmtfs"
copy "$source/LICENSE" usr/share/licenses/rmtfs/LICENSE
copy "$port/SYNAPSE.md" usr/share/doc/rmtfs/SYNAPSE.md
