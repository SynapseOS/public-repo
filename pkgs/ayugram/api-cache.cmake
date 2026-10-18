# api-cache.cmake: передать реквизиты приложения из локального окружения
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: сборка графических пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный, MIT

set(TDESKTOP_API_ID "$ENV{TDESKTOP_API_ID}" CACHE STRING "Local application ID" FORCE)
set(TDESKTOP_API_HASH "$ENV{TDESKTOP_API_HASH}" CACHE STRING "Local application hash" FORCE)
set(TDESKTOP_API_TEST OFF CACHE BOOL "Test credentials are not used" FORCE)
