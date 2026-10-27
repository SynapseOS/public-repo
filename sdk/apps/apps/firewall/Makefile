# Makefile: сборка пользовательского управления firewall
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: пользовательские пакеты
# Первое изменение: 18.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный
.PHONY: all
all:
	+$(MAKE) -C ../../../.. firewall

	mkdir -p ../apps-bin
	cp ../../../../pkgs/.work/firewall/payload/bin/firewall ../apps-bin/firewall
