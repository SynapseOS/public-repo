# NetSurf

NetSurf 3.11 с framebuffer frontend, SDL2 и встроенным Duktape 2.7.0
Окно SDL2 может работать внутри Weston

```sh
make netsurf JOBS=4
```

Запуск из терминала Weston в SynapseOS:

```sh
netsurf https://synapseos.ru
```

JavaScript включён по умолчанию. Порт добавляет Promise, текстовые fetch,
асинхронный XMLHttpRequest и постоянный localStorage с разделением по origin
Сеть обслуживается через curl multi в цикле браузера, TLS проверяется
Поддерживаются простые CORS GET/HEAD без credentials и пользовательских заголовков
CORS preflight, перенаправления fetch и двоичные тела не реализованы
XHR поддерживает text/json, отмену и таймауты, синхронные запросы не поддерживаются

Duktape не поддерживает весь современный синтаксис JavaScript
Добавление сетевых API не гарантирует работу приложений со стрелочными функциями,
модулями и современными средствами отображения. Полноценная работа GitHub и других
сложных сайтов не заявляется

Код расширений находится в `synapse-web.inc` и `synapse-web.js`, патчи upstream
хранятся в `patches/`, шаги сборки в `recipe.sh`
`test-javascript.py` и `test-web.js` служат проверкам браузера и не участвуют в сборке
Сетевой тестовый сервер можно запускать отдельно, его справка доступна через `--help`
