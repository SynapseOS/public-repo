# Makefile: Отдельная сборка инструментов synbench для двух ОС
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
# Подсистема: сборка пользовательских измерений
# Первое изменение: 19.10.2026, последнее изменение: 25.10.2026
# Доступ: общедоступный

HOST_CC ?= cc
CC ?= cc
GUEST_CC ?= ../common/musl-cc
PORTABLE_CC ?= musl-gcc
PUBLIC_PACKAGES_ROOT ?= $(abspath ../../../..)
QWEN_SOURCE ?= $(abspath ../qwen3.c/runq.c)
OUT ?= build
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow
QWEN_FLAGS := $(CFLAGS)

.PHONY: all host guest qwen portable package
all: host guest
host: $(OUT)/linux/synbench $(OUT)/linux/synbench-workload
guest: $(OUT)/synapse/synbench $(OUT)/synapse/synbench-workload
qwen: $(OUT)/linux/synbench-qwen $(OUT)/synapse/synbench-qwen
portable: $(OUT)/portable/synbench $(OUT)/portable/synbench-workload $(OUT)/portable/synbench-qwen

$(OUT)/portable/synbench: guest.c Makefile
	@mkdir -p $(@D)
	$(PORTABLE_CC) -static $(CFLAGS) $< -o $@

$(OUT)/portable/synbench-workload: workload.c Makefile
	@mkdir -p $(@D)
	$(PORTABLE_CC) -static $(CFLAGS) -pthread -fno-builtin-memcpy $< -o $@

$(OUT)/portable/synbench-qwen: qwen.c $(QWEN_SOURCE) Makefile
	@mkdir -p $(@D)
	$(PORTABLE_CC) -static $(QWEN_FLAGS) -DQWEN_SOURCE='"$(QWEN_SOURCE)"' $< -lm -o $@

$(OUT)/linux/synbench: guest.c Makefile
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS) $< -o $@

$(OUT)/synapse/synbench: guest.c Makefile
	@mkdir -p $(@D)
	$(GUEST_CC) $(CFLAGS) -Wl,--build-id=none $< -o $@

$(OUT)/linux/synbench-workload: workload.c Makefile
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS) -pthread -fno-builtin-memcpy $< -o $@

$(OUT)/synapse/synbench-workload: workload.c Makefile
	@mkdir -p $(@D)
	$(GUEST_CC) $(CFLAGS) -pthread -Wl,--build-id=none -fno-builtin-memcpy $< -o $@

# Исключения для предупреждений upstream ограничены его включением в qwen.c
$(OUT)/linux/synbench-qwen: qwen.c $(QWEN_SOURCE) Makefile
	@mkdir -p $(@D)
	$(HOST_CC) $(QWEN_FLAGS) -DQWEN_SOURCE='"$(QWEN_SOURCE)"' $< -lm -o $@

$(OUT)/synapse/synbench-qwen: qwen.c $(QWEN_SOURCE) Makefile
	@mkdir -p $(@D)
	$(GUEST_CC) $(QWEN_FLAGS) -Wl,--build-id=none -DQWEN_SOURCE='"$(QWEN_SOURCE)"' $< -lm -o $@

# Использовать действующий упаковщик, все новые результаты остаются в OUT
package:
	ROOT='$(PUBLIC_PACKAGES_ROOT)' SDK='$(PUBLIC_PACKAGES_ROOT)/.sdk' \
	BUILD_ROOT='$(abspath $(OUT))/package-work' OUT='$(abspath $(OUT))/packages' \
	bash -eu -o pipefail -c 'source "$$ROOT/build/common.sh"; \
	name=synbench; port="$$PWD"; work=$$(mktemp -d /tmp/synbench-package-XXXXXXXX); \
	payload="$$work/payload"; mkdir -p "$$payload/bin"; \
	source "$$port/recipe.sh"; pack'
