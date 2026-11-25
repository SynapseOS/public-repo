# qrtr-glib

Настоящая upstream библиотека GLib/GIO для QRTR 1.4.0; не заменяет BSD `libqrtr`
и не предоставляет тот же API. Статическая сборка поддерживает x86_64 и ARM64.
ARM64: `bash build/qmi-stack.sh` из корня public-repo.

Для выполнения нужны AF_QIPCRTR (42), datagram socket API, GLib main loop и
публикация QRTR сервисов со стороны модема. Introspection и документация GTK отключены.
Исходники: https://github.com/linux-mobile-broadband/libqrtr-glib/tree/1.4.0
