# recipe.sh: закреплённый BSD tqftpserv со штатной libqrtr
# Автор: Елчинян Арен Артурович 2026
# Архитектура: AArch64 и x86_64, ABI: musl и AF_QIPCRTR
# Описание: Upstream TFTP не изменён, заменена только граница файловой системы
# Подсистема: публичные пакеты
# Первое изменение: 25.11.2026, последнее изменение: 25.11.2026
# Доступ: общедоступный
prepare
if [[ -n ${TQFTPSERV_CC:-${QRTR_CC:-}} ]]; then
    CC=${TQFTPSERV_CC:-$QRTR_CC}
fi
qrtr=${LIBQRTR_ROOT:-$BUILD_ROOT/libqrtr/payload/usr}
[[ -f $qrtr/include/libqrtr.h && -f $qrtr/lib/libqrtr.a ]]
"$CC" -O2 -Wall -Wextra -Werror -D__synapse__ -ffunction-sections -fdata-sections \
    -I"$qrtr/include" -I"$source" "$source/tqftpserv.c" "$port/synapse-translate.c" \
    "$qrtr/lib/libqrtr.a" -static -Wl,--gc-sections -o "$payload/bin/tqftpserv"
copy "$source/LICENSE" usr/share/licenses/tqftpserv/LICENSE
copy "$qrtr/share/licenses/libqrtr/LICENSE" usr/share/licenses/tqftpserv/libqrtr-LICENSE
copy "$port/synapse-translate.c" usr/share/licenses/tqftpserv/synapse-translate.c
