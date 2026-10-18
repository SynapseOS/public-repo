# Mindustry v160.7

Пакет содержит сервер и клиент из официального релиза v160.7.
Файлы игры: `/usr/share/games/mindustry/server-release.jar` и
`/usr/share/games/mindustry/Mindustry.jar`. Общий каталог игр —
`/usr/share/games`.

Установка: `setup install mindustry`. Зависимость — пакет `openjdk`.
Сервер можно запускать напрямую через `java -jar` (команда ниже), клиент — `mindustry`.
В cloud используйте `mindustry-server` или `java -jar server-release.jar`.
`Mindustry.jar` содержит графический клиент. С установленным `mindustry-desktop`
его можно запускать обычной командой `java -jar Mindustry.jar`, включая копию
с другим именем или путём с пробелами. Системная Java автоматически подключает
совместимость для официального JAR v160.7, сам JAR остаётся неизменным.
Обновляйте пакеты `openjdk` и `mindustry-desktop` вместе.
Команда `mindustry` также доступна. В cloud графические пакеты не предустановлены.
Аргументы передаются игре. Для сети launcher выбирает poll selector JDK. Команды сервера: `help`, `host`, `status`, `exit`.

Рабочие каталоги сервера и клиента: `/var/lib/mindustry/server` и
`/var/lib/mindustry/client`. Они создаются при запуске и не входят в пакет:
обновление JAR не заменяет карты, настройки, моды и сохранения.
Свой каталог можно задать переменной `MINDUSTRY_DATA_DIR` (родитель должен
существовать). Для графического клиента установите `mindustry-desktop` в desktop или extended:
он добавляет Mesa/OpenGL, SDL и звуковой модуль. Запуск — `mindustry` после
закрытия рабочего стола; подробности в [инструкции OpenGL](opengl.md).

## Команды запуска текущего стенда

В установленной SynapseOS:

```sh
setup install mindustry
mkdir -p /var/lib/mindustry/server
cd /var/lib/mindustry/server
java -Djava.nio.channels.spi.SelectorProvider=sun.nio.ch.PollSelectorProvider \
  -Djava.awt.headless=true -Xms32m -Xmx512m \
  -jar /usr/share/games/mindustry/server-release.jar \
  "config antiSpam true,config strict true,config messageRateLimit 1,config autosave true,config autosaveSpacing 60,host Fork"
```

`status` показывает состояние, `save 1` сохраняет игру, `exit` завершает сервер.

