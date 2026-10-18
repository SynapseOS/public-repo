# Makefile: сборка публичных пакетов SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
JOBS ?= 4
BUILD_ROOT ?= $(ROOT)/pkgs/.work
OUT ?= $(ROOT)/out
SDK ?= $(ROOT)/.sdk
export PATH := $(BUILD_ROOT)/host-tools/root/usr/bin:$(PATH)
export BISON_PKGDATADIR := $(BUILD_ROOT)/host-tools/root/usr/share/bison
export M4 := $(BUILD_ROOT)/host-tools/root/usr/bin/m4
unexport CC CXX AR LD NASM NASMFLAGS COMMON_CFLAGS COMMON_CPPFLAGS COMMON_LDFLAGS
export ROOT JOBS BUILD_ROOT OUT SDK
PACKAGES := $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard $(ROOT)/pkgs/*/manifest.json)))))
SDK_INPUTS := $(shell find $(ROOT)/sdk -type f)
BUILD_INPUTS := $(wildcard $(ROOT)/build/*.sh) $(ROOT)/build/TrustStore.java
.PHONY: all list check check-tools host-tools sdk clean $(PACKAGES)
all: $(PACKAGES)
list:
	@printf '%s\n' $(PACKAGES)
check-tools:
	@for tool in bash jq curl sha256sum tar bsdtar patch flock make gcc readelf; do \
		command -v $$tool >/dev/null; \
	done
check: check-tools
	@for script in build/*.sh pkgs/*/recipe.sh; do \
		bash -n "$$script"; \
	done
	@for spec in pkgs/*/*.json; do \
		jq empty "$$spec"; \
	done
	@bash build/check.sh
host-tools: check-tools
	@bash "$(ROOT)/build/host-tools.sh"
sdk: $(SDK)/.ready
$(SDK)/.ready: $(SDK_INPUTS) $(ROOT)/build/sdk.sh | check-tools
	@bash "$(ROOT)/build/sdk.sh"
	@touch "$@"
clean:
	@echo 'Удаляйте нужные каталоги кэша и out явно, общая очистка отключена'

define package_rule
$(1): $(BUILD_ROOT)/$(1)/.built
$(BUILD_ROOT)/$(1)/.built: $(shell find $(ROOT)/pkgs/$(1) -type f) $(BUILD_INPUTS) $(SDK)/.ready | host-tools
	@mkdir -p "$(BUILD_ROOT)/$(1)"
	@bash "$(ROOT)/build/package.sh" "$(1)"
	@touch "$$@"
endef
$(foreach package,$(PACKAGES),$(eval $(call package_rule,$(package))))
include $(ROOT)/build/dependencies.mk
