# BSD RMTFS для SynapseOS

Исходники linux-msm/rmtfs закреплены на `b30a3eb38f9af283f18dbd3c7755653efc52c094` под BSD-3-Clause

Пакет сохраняет upstream QMI RMTFS service14 и кодеки libqrtr
Maintained patch ограничивает хранилище private RAM shadow и исправляет проверки границ
Никакой доступ к постоянным разделам телефона в пакет не входит

Запуск в отдельном QRTR boot profile: `rmtfs -r -o /etc/phone-nv`
`-r` включён также по умолчанию, изменения существуют только в RAM shadow
Разрешены только `modem_fs1`, `modem_fs2`, `modem_fsg`, `modem_fsc`
Каждый файл должен быть обычным, ненулевым, не более16MiB, суммарные RAM shadows не более64MiB
`-P`, `-s`, другие каталоги и неизвестные имена отвергаются
NV файлы и их содержимое в публичный пакет не включаются

Shared-memory boundary открывает только `/dev/qcom-rmtfs`
PREPARE, GETINFO и mmap offset0 подтверждают ABI1, READY и guarded 2MiB buffer
Публичный physical address берётся только из GETINFO
ALLOC0 выбирает global relative offset namespace, ALLOC>0 — physical namespace
Ответ ALLOC0 сохраняет physical base, как наблюдалось в native55
Смена namespace после выбора запрещена, арифметика проверяется до копирования
Ядро сохраняет ownership общего буфера после выхода daemon до перезагрузки телефона

Linux libudev, sysfs, UIO, remoteproc и `/dev/mem` в собранном бинарнике отсутствуют
Lifecycle MSS и exclusive GLINK lease принадлежат общему ядру
Одновременно с legacy RAW NV daemon запускать нельзя

Пакет проверяется host boundary tests и AArch64 линковкой
Совместимость нового upstream handler с реальным MPSS требует отдельного native запуска
