# libqmi и qmicli

Закреплён upstream 1.38.0; полный набор сообщений, включая UIM, DMS, NAS, WMS и VOICE.
Включён транспорт QRTR через отдельный пакет `qrtr-glib`.
Библиотека LGPL и программы GPL собираются в пользовательское пространство.

На ARM64 используется уже установленный musl sysroot; новая полная SDK не создаётся:

```sh
bash build/qmi-stack.sh
```

Результат — `work/pixel3a-port/qmi-stack/out` относительно корня SynapseOS.
Пути sysroot, linker и рабочего каталога можно задать переменными `QMI_SYSROOT`,
`QMI_GCC_DIR`, `QMI_LLD`, `QMI_STRIP`, `QMI_WORK_ROOT`, `QMI_STACK_ROOT`.
Поддерживается повторная сборка отдельных пакетов, например `bash build/qmi-stack.sh libqmi`.
Зависимости в этом случае должны быть уже собраны в том же рабочем каталоге.

Проверка самого CLI без обращения к модему: `qmicli --version`, затем `qmicli --help-all`.
Реальное обнаружение требует работающего AF_QIPCRTR (42) в ядре и доступных QMI сервисов.
Успешная кросс-сборка сама по себе не подтверждает работу SIM, SMS и сети на устройстве.
`qmi-network` — upstream shell helper, которому дополнительно нужны совместимые shell,
`ip`, DHCP и драйвер сетевого канала; его установка не означает готовность мобильного интернета.

Сборка отключает дополнительные инструменты обновления firmware, MBIM, RMNET, udev,
проверку конкурирующего ModemManager и introspection. Основные QMI сервисы сохранены.
Исходники: https://github.com/linux-mobile-broadband/libqmi/tree/1.38.0
