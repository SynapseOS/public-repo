#!/bin/sh
# build-in-root.sh: собрать AyuGram в отдельном окружении musl
# Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
# Подсистема: сборка графических пакетов
# Первое изменение: 18.10.2026, последнее изменение: 19.10.2026
# Статус доступа: общедоступный, MIT

set -eu
umask 077

cmake -S /work/tde2e-src -B /work/tde2e-out -G Ninja \
	-DCMAKE_BUILD_TYPE=MinSizeRel \
	-DCMAKE_INSTALL_PREFIX=/work/tde2e-install \
	-DTD_E2E_ONLY=ON
cmake --build /work/tde2e-out --parallel "${JOBS:-2}"
cmake --install /work/tde2e-out

cmake -S /work/ayu-src -B /work/out -G Ninja \
	-C /port/api-cache.cmake \
	-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=/port/build-pools.cmake \
	-DCMAKE_BUILD_TYPE=MinSizeRel \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-Dtde2e_DIR=/work/tde2e-install/lib/cmake/tde2e \
	-DDESKTOP_APP_USE_PACKAGED=ON \
	-DDESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
	-DDESKTOP_APP_DISABLE_AUTOUPDATE=ON \
	-DDESKTOP_APP_ENABLE_LTO=OFF \
	-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,stack-size=1024768

cmake --build /work/out --target Telegram ayugram-sqlite-probe ayugram-async-probe --parallel "${JOBS:-2}"
strip /work/out/bin/AyuGram
