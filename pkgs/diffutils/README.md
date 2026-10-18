# diffutils

Закреплённая версия, URL и SHA-256 находятся в [port.json](port.json)
Патчи хранятся в `patches/`, если они нужны этому порту

Из корня репозитория:

```sh
make diffutils JOBS=4
```

Можно использовать `make -C pkgs/diffutils`
Результат находится в `out/`, общие требования описаны в [README](../../README.md)
Программа собирается с musl, проверка исполнения требует запуска в SynapseOS
