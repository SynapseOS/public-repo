# recipe.sh: Предоставить интерфейс OpenGL через закреплённую Mesa
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

copy "$BUILD_ROOT/mesa/stage/usr/lib/libGL.so.1" usr/lib/libGL.so.1
copy "$BUILD_ROOT/mesa/stage/usr/lib/libGL.so.1" usr/lib/libGL.so
copy "$SDK/docs/user/opengl.md" usr/share/doc/opengl/README.md
