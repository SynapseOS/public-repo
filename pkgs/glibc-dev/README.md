# Файлы разработки glibc

Пакет содержит заголовки glibc и Linux UAPI, стартовые объектные файлы,
статические библиотеки и файлы для динамической линковки. Всё размещено в
`/usr/share/glibc/sysroot`. Общие `/usr/include` и `/usr/lib` остаются за
основной средой musl. Пакет зависит от точно соответствующего runtime glibc.

Сборка из корня репозитория:

```sh
make glibc-dev JOBS=2
```

Для программы с glibc нужно явно выбрать этот sysroot. Проверенная линковка
`smoke.c` на хосте:

```sh
sysroot=pkgs/.work/glibc-dev/payload/usr/share/glibc/sysroot
gcc -O2 -Wall -Wextra -Werror --sysroot="$sysroot" -B"$sysroot/usr/lib/" \
    -nostdinc -isystem "$(gcc -print-file-name=include)" \
    -isystem "$sysroot/usr/include" \
    -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link="$sysroot/lib" \
    -L"$sysroot/usr/lib" -L"$sysroot/lib" \
    pkgs/glibc-dev/smoke.c -pthread -ldl -lm -o /tmp/glibc-smoke
readelf -l -V /tmp/glibc-smoke
```

В тестовой QEMU VM с ядром 0.1.948 и runtime 2.44.0-r6 этот ELF завершился
с кодом 0 и вывел `GLIBC_SMOKE_PASS`. Исходник проверяет потоки, TLS, NSS,
локали `C`, `en_US.UTF-8`, `ru_RU.UTF-8`, файл, TCP через loopback, внешний
DNS и `dlopen`. Другие программы и локали требуют отдельной проверки.
