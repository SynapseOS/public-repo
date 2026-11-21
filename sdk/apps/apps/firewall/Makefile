# Makefile: сборка пользовательского управления firewall
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: пользовательские пакеты
# Первое изменение: 18.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный
PUBLIC_PACKAGES_ROOT ?= $(abspath ../../../..)
BUILD_ROOT ?= $(PUBLIC_PACKAGES_ROOT)/pkgs/.work
.PHONY: all
all:
	+$(MAKE) -C "$(PUBLIC_PACKAGES_ROOT)" firewall

	mkdir -p ../apps-bin
	cp $(BUILD_ROOT)/firewall/payload/bin/firewall ../apps-bin/firewall
