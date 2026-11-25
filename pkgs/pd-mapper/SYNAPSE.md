# BSD pd-mapper для SynapseOS

Upstream linux-msm/pd-mapper закреплён на `5ecd2fe926aca7abfe40724177f63b942cff3947`, BSD-3-Clause
QMI service64, описание services и XZ decoder взяты из upstream с maintained patch

Платформа перечисляет только настоящие `.jsn` и `.jsn.xz` в `/lib/firmware`
Рекурсивный обход не проходит через symlinks, глубина ограничена8
Лимиты discovery: 256каталогов, 8192entries, 256manifests, 4MiB на файл
После XZ распаковки предел4MiB, decoder memory limit96MiB
Временный распакованный файл создаётся в `TMPDIR` либо `/tmp` и сразу unlink
JSON parser имеет предел32 вложений, registry — 512service maps
QMI GET_DOMAIN_LIST отвечает страницами по16 доменов с offset и полным total_domains

Пакет не содержит придуманных карт или закрытых firmware manifests
Без настоящих карт daemon завершится до открытия QRTR socket и публикации службы
Linux `/sys/class/remoteproc` и firmware_class paths не используются

Наличие пакета и прохождение host tests не подтверждают native service discovery Pixel
В текущем root56 нет настоящих `.jsn`, их необходимо отдельно получить из прошивки телефона
Запуск daemon на телефоне проверяется после установки этих файлов
