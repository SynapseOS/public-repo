// synapse-rproc.c: Lifecycle модема принадлежит общему ядру SynapseOS
// Автор: Елчинян Арен Артурович 2026
// Архитектура: AArch64 и x86_64, ABI: BSD RMTFS boundary
// Описание: Linux remoteproc sysfs запрещён, никаких автоматических reset или start
// Подсистема: публичные пакеты телефона
// Первое изменение: 25.11.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный
#include <errno.h>
int rproc_init(void) { errno = EOPNOTSUPP; return -1; }
int rproc_start(void) { errno = EOPNOTSUPP; return -1; }
int rproc_stop(void) { errno = EOPNOTSUPP; return -1; }
