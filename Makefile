# Makefile: сборка публичных пакетов SynapseOS
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 25.10.2026
# Статус доступа: общедоступный
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.SUFFIXES:
JOBS ?= 4
APPS ?=
export PUBLIC_PACKAGES_ROOT := $(ROOT)
ARCH ?= x86_64
ifneq ($(filter arm64 aarch64,$(ARCH)),)
TARGET_ARCH := aarch64
ARCH_SUFFIX := -aarch64
else ifneq ($(filter amd64 x86_64,$(ARCH)),)
TARGET_ARCH := x86_64
else
$(error Unsupported ARCH=$(ARCH))
endif
export TARGET_ARCH
BUILD_ROOT ?= $(ROOT)/pkgs/.work$(ARCH_SUFFIX)
OUT ?= $(ROOT)/out$(ARCH_SUFFIX)
SDK ?= $(ROOT)/.sdk$(ARCH_SUFFIX)
REPO_DIR ?= $(OUT)/repo
REPO_ID ?= official
GENERATION ?=
TTL ?= 2592000
SIGNING_PUBLIC_KEY ?= $(HOME)/.ssh/id_ed25519.pub
PACKAGE_FILES ?=
REPOSITORY_CC ?= cc
REPOSITORY_TOOL := $(BUILD_ROOT)/host-tools/repository
export REPO_DIR REPO_ID GENERATION TTL SIGNING_PUBLIC_KEY REPOSITORY_TOOL
export PATH := $(BUILD_ROOT)/host-tools/root/usr/bin:$(PATH)
export BISON_PKGDATADIR := $(BUILD_ROOT)/host-tools/root/usr/share/bison
export M4 := $(BUILD_ROOT)/host-tools/root/usr/bin/m4
unexport CC CXX AR LD NASM NASMFLAGS COMMON_CFLAGS COMMON_CPPFLAGS COMMON_LDFLAGS
export ROOT JOBS BUILD_ROOT OUT SDK
PACKAGES := $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard $(ROOT)/pkgs/*/manifest.json)))))
SDK_INPUTS := $(shell find $(ROOT)/sdk -type f)
BUILD_INPUTS := $(filter-out $(ROOT)/build/repository.sh $(ROOT)/build/repository-test.sh,$(wildcard $(ROOT)/build/*.sh)) $(ROOT)/build/TrustStore.java $(ROOT)/build/package-policy.jq
.PHONY: all list check check-tools host-tools sdk clean repository publish check-signing signing-key $(PACKAGES)
all: native $(PACKAGES)
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
host-tools:
	@bash "$(ROOT)/build/host-tools.sh"
sdk: $(SDK)/.ready
$(SDK)/.ready: $(SDK_INPUTS) $(ROOT)/build/sdk.sh $(ROOT)/Makefile | check-tools host-tools
	@bash "$(ROOT)/build/sdk.sh"
	@touch "$@"
clean:
	@echo 'Удаляйте нужные каталоги кэша и out явно, общая очистка отключена'

# Сначала завершить сборку даже при запуске make с несколькими заданиями
repository: all
	+$(MAKE) publish

# Подписать уже собранные архивы без повторной сборки пакетов
publish: $(REPOSITORY_TOOL)
	@bash "$(ROOT)/build/repository.sh" $(foreach archive,$(PACKAGE_FILES),"$(archive)")

check-signing: $(REPOSITORY_TOOL)
	@bash "$(ROOT)/build/repository-test.sh"

signing-key: $(REPOSITORY_TOOL)
	@"$(REPOSITORY_TOOL)" key "$(SIGNING_PUBLIC_KEY)"

$(REPOSITORY_TOOL): $(ROOT)/build/repository.c
	@mkdir -p "$(@D)"
	$(REPOSITORY_CC) -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow \
		-o "$@.new" "$<" -lcrypto
	@mv "$@.new" "$@"

define package_rule
$(1): $(BUILD_ROOT)/$(1)/.built
	@payload_dir="$(BUILD_ROOT)/$(1)/payload"; \
	lock_dir="$(BUILD_ROOT)/$(1)"; \
	if [ "$(1)" = wayland-client ]; then \
		payload_dir="$(BUILD_ROOT)/weston/client-payload"; \
		lock_dir="$(BUILD_ROOT)/weston"; \
	elif [ "$(1)" = xkeyboard-config ]; then \
		payload_dir="$(BUILD_ROOT)/weston/xkb-payload"; \
		lock_dir="$(BUILD_ROOT)/weston"; \
	fi; \
	verify_payload() { \
		( flock 9 && \
			test -s "$(OUT)/$(1).manifest.json" && \
			jq -er '.files[] | select(.type == "file") | "\(.sha256)  \(.path)"' \
				"$(OUT)/$(1).manifest.json" | \
				(cd "$$$$payload_dir" && sha256sum --check --status) \
		) 9>"$$$$lock_dir/build.lock"; \
	}; \
	if ! verify_payload; then \
		$(MAKE) --no-print-directory -W "$(ROOT)/pkgs/$(1)/port.json" \
			"$(BUILD_ROOT)/$(1)/.built"; \
		verify_payload; \
	fi
$(BUILD_ROOT)/$(1)/.built: $(shell find $(ROOT)/pkgs/$(1) -type f) $(BUILD_INPUTS) $(SDK)/.ready | host-tools
	@mkdir -p "$(BUILD_ROOT)/$(1)"
	@bash "$(ROOT)/build/package.sh" "$(1)"
	@touch "$$@"
endef
$(foreach package,$(PACKAGES),$(eval $(call package_rule,$(package))))
include $(ROOT)/build/dependencies.mk

# Нативные приложения используют тот же открытый SDK, что и пакетные рецепты
.PHONY: native native-libs
native-libs: sdk
	+$(MAKE) -C "$(SDK)/apps/synlibc" -j"$(JOBS)"
	+$(MAKE) -C "$(SDK)/apps/libs" -j"$(JOBS)"
native: native-libs
	+flock "$(SDK)/native.lock" $(MAKE) -C "$(SDK)/apps/apps" -j"$(JOBS)" $(APPS)
