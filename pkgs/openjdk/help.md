# OpenJDK 21

`setup install openjdk` устанавливает headless OpenJDK 21.0.11 для x86_64/musl.
Запуск: `java -version`, `java -jar файл.jar`. Runtime находится в
`/usr/lib/jvm/java-21-openjdk`; `/bin/java` задаёт пути его shared-библиотек.
Пакет не содержит javac и графический backend AWT/X11.

С установленным `mindustry-desktop` команда `java -jar Mindustry.jar`
автоматически подключает модуль `/usr/share/java/synapse-compat.jar`.
Подходящий JAR выбирается по классам ZIP, затем модуль проверяет Main-Class
и SHA-256 официального клиента v160.7. Имя и расположение файла могут меняться.
Модуль настраивает Linux ABI для этого клиента и загрузку библиотек musl.
Для остальных программ настройки окружения и выбор библиотек не меняются.
`java -version` и серверный JAR запускаются без этого модуля.

Основа - pinned пакеты Alpine 3.22; URL и SHA-256 записаны в
`pkgs/openjdk/port.json`. Файлы лицензий OpenJDK включены в каталог `legal`.
Загрузчик и libc предоставляет `musl-runtime` SynapseOS.

Работа JVM проверяется в гостевой ОС. Для сетевых Java-приложений временно
можно выбрать poll selector:
`java -Djava.nio.channels.spi.SelectorProvider=sun.nio.ch.PollSelectorProvider …`.
SSL trust store `lib/security/cacerts` генерируется из системного PEM
CA bundle; пароль стандартного JKS - `changeit` (хранилище содержит только
публичные сертификаты).

Пакет занимает около 194 MiB, поэтому требуется `setup >= 0.1.512-r6`
с лимитом архивов 256 MiB. На старой системе сначала обновите сам менеджер:
`setup install setup`, затем устанавливайте `openjdk` или `mindustry`.
Требуется ядро `>= 0.1.645-r1` с корректным statx и исправлениями futex.
