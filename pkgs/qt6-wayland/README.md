# Qt Wayland

```sh
make qt6-wayland JOBS=2
```

Qt Wayland 6.11.1 для запуска Qt-приложений внутри Weston SynapseOS
Пакет содержит модуль QPA, поддержку xdg-shell и библиотеки Qt Wayland
Изображение передаётся через общую память, EGL и DRM для первого запуска не требуются
Наличие библиотек OpenGL в зависимостях не означает поддержку ускорения в гостевой ОС

Используются закреплённые musl APK из `assets.json`
Адаптированные Wayland client/server и XKB берутся из пакета Weston
Исходные библиотеки Alpine Wayland не подменяют реализацию SynapseOS
Полная сборка Qt Wayland из исходников пока не реализована
Гостевая проверка выполняется командой `qt6-smoke --self-test` из терминала Weston
