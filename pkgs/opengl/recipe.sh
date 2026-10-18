# recipe.sh: Предоставить интерфейс OpenGL через закреплённую Mesa
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

copy "$BUILD_ROOT/mesa/payload/usr/lib/libOSMesa.so.8" usr/lib/libGL.so.1
copy "$SDK/docs/user/opengl.md" usr/share/doc/opengl/README.md
