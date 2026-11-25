# recipe.sh: BSD service registry на настоящих firmware manifests
# Автор: Елчинян Арен Артурович 2026
# Архитектура: AArch64 и x86_64, ABI: musl и AF42
# Описание: Сохраняет QMI и XZ upstream, без Linux remoteproc fallback
# Подсистема: публичные пакеты телефона
# Первое изменение: 25.11.2026
# Последнее изменение: 25.11.2026
# Доступ: общедоступный
prepare
CC=${PD_MAPPER_CC:-$CC}
qrtr=${PD_MAPPER_QRTR_STAGE:?Set PD_MAPPER_QRTR_STAGE to matching libqrtr payload}
lzma=${PD_MAPPER_LZMA_STAGE:?Set PD_MAPPER_LZMA_STAGE to matching liblzma payload}
flags=(-O2 -D_GNU_SOURCE -DSYNAPSE_PD_MAPPER=1 -ffunction-sections -fdata-sections
    -I"$port" -I"$source" -I"$qrtr/usr/include" -I"$lzma/usr/include")
objects=()
for unit in pd-mapper assoc json servreg_loc lzma_decomp; do
    "$CC" "${flags[@]}" -c "$source/$unit.c" -o "$work/$unit.o"
    objects+=("$work/$unit.o")
done
"$CC" -static -Wl,--gc-sections "${objects[@]}" "$qrtr/usr/lib/libqrtr.a" \
    "$lzma/usr/lib/liblzma.a" -o "$payload/bin/pd-mapper"
copy "$source/LICENSE" usr/share/licenses/pd-mapper/LICENSE
copy "$port/SYNAPSE.md" usr/share/doc/pd-mapper/SYNAPSE.md
