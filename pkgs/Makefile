# Совместимый вход в общую сборку, рецепты находятся в корневом Makefile
.DEFAULT_GOAL := all
.PHONY: all
all:
	+$(MAKE) -C .. all
%:
	+$(MAKE) -C .. $@
