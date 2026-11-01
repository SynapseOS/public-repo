# Makefile: сборка APT для отдельного Debian-окружения
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: x86_64, ABI: synapse-x86_64-v1
# Подсистема: сборка пакетов
# Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
# Статус доступа: общедоступный

.DEFAULT_GOAL := all
.PHONY: all check-signature

all:
	+$(MAKE) -C ../.. apt

check-signature:
	+$(MAKE) -C ../.. debian-archive-keyring
	@bash check-signature.sh
