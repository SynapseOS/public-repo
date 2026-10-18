# Weston

```sh
make weston JOBS=4
```

Пакет содержит Weston 13.0.3 с backend для framebuffer и ввода SynapseOS
Wayland client/cursor и карты клавиатуры поставляются отдельными пакетами
Они собираются той же целью автоматически

В SynapseOS нужен интерфейс ядра версии не ниже 0.1.907
Запуск: `weston`, затем `weston-terminal` или `weston-simple-shm`
Из терминала Weston можно запускать `netsurf` и `java -jar Mindustry.jar`
Возврат в консоль: Ctrl+Alt+Esc

Используется программный рендерер Pixman, один экран XRGB8888, масштаб 1
EGL, DRM/KMS и Xwayland отключены. Тема курсора Breeze общая для приложений,
раскладки us/ru переключаются через Alt+Shift
Экранная клавиатура отключена настройкой `[input-method] path=` в `weston.ini`
Сборка пакета не заменяет проверку отображения и ввода в гостевой ОС

Адаптации Wayland хранятся в `patches/` и `*.inc`
Реализация локальных сокетов ядра в этот репозиторий не входит
