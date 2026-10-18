# Публичные пакеты SynapseOS

[![Build public packages](https://github.com/SynapseOS/public-repo/actions/workflows/packages.yml/badge.svg)](https://github.com/SynapseOS/public-repo/actions/workflows/packages.yml)

Рецепты сборки пакетов под SynapseOS

Сборкой управляет GNU Make. Загрузка, проверка SHA-256, применение патчей и упаковка
выполняются shell-рецептами. Собственных Python-сборщиков в дереве пакетов нет
Python нужен upstream-проектам Meson, Mako и CPython, их штатные генераторы сохранены

## Зависимости хоста

Нужен Linux x86_64, GNU Make, GCC/G++ 14, binutils, curl, jq, libarchive-tools, patch,
pkg-config, autoconf, automake, libtool, bison, flex, gperf, CMake, Ninja, Meson,
Mako, Perl, NASM, zip/unzip, JDK 21 и заголовки host libpng
Рецепты рассчитаны на Meson 1.7.2. CI устанавливает эту версию отдельно
Для сборки пакета CPython требуется host CPython 3.13, при его отсутствии рецепт
собирает его из того же закреплённого исходного архива

Пример установки системных инструментов на Debian 13:

```sh
sudo apt-get install build-essential musl-tools curl jq libarchive-tools patch pkg-config \
  autoconf automake libtool bison flex gperf cmake ninja-build meson python3-mako \
  python3-yaml python3-packaging perl nasm unzip zip libwayland-dev libpng-dev \
  libgmp-dev libmpfr-dev libmpc-dev openjdk-21-jdk-headless
```

## Сборка

```sh
make check
make all JOBS=4
make list
make netsurf JOBS=4
make -C pkgs/sqlite
```

`make` по умолчанию собирает все пакеты. `JOBS` задаёт число процессов внутри одной
сборки. Не задавайте одновременно большие значения `make -j` и `JOBS`
Пакет `musl-tools` предоставляет host-загрузчик для закреплённых инструментов LLVM
SDK собирается автоматически из `sdk/`, закрытый репозиторий и SSH-ключи не нужны
Для SDK, кэша и готовых архивов используются `.sdk`, `pkgs/.work` и `out`
Каталоги `apps`, `include` и `assets` в корне создаются как локальные ссылки на SDK

Архивы `out/<sha256>.spkg` содержат `manifest.json` и файлы `payload/` в формате USTAR
Для каждого пакета сохраняется `out/<имя>.manifest.json`
SHA-256 проверяется при каждом использовании исходного архива, включая кэш
Манифест содержит не более 512 файлов
Данные больших наборов Nmap и Open POSIX разбиты на отдельные пакеты
Чужие образы, логи и кэши не удаляются командой общей очистки

## CI и проверки

Workflow `.github/workflows/packages.yml` использует Debian 13 и выполняет `make check`, затем полную
сборку `make all JOBS=2` и сохраняет пакеты как артефакт. Он не публикует бинарники
в репозиторий обновлений и не использует секреты основного проекта
Локальная сборка и успешный CI не подтверждают работу программы в гостевой ОС
Графику, звук, сеть и пользовательский ввод нужно проверять в отдельной SynapseOS

## Исходники и лицензии

`pkgs/<имя>/` содержит манифест, закреплённые источники, патчи, Makefile и при
необходимости `recipe.sh`. Общая сборочная обвязка находится в `build/`
`sdk/` содержит исходники пользовательских библиотек, приложений и определения ABI
Реализации драйверов, локальных сокетов и фильтра ядра сюда не включены
Пользовательская утилита firewall имеет отдельную проверку конфигурации

Условия распространения определяются лицензиями соответствующих компонентов
Заголовки upstream, COPYRIGHT, COPYING и LICENSE сохранены. Единую лицензию
на все исходники репозитория этот файл не устанавливает
Поле `upstream_license` в `packages.json` отражает данные рецепта, `null` означает,
что отдельное поле лицензии в рецепте отсутствует
Данные Doom WAD не поставляются, права на них проверяются отдельно
Коммерческие зависимости исполнения не требуются для
компиляции публичных клиентских пакетов и остаются внешними пакетами

При портировании ПО использовались ИИ технологии.
Использование ИИ в коде драйверов и ядра запрещено.