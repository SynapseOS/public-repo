TARGET := diskhealth
SOURCES := main.c report.c report.h ../../../include/lumine/diskhealth.h
.PHONY: all clean
all: ../apps-bin/$(TARGET)
../apps-bin/$(TARGET): $(SOURCES)
	@mkdir -p ../apps-bin
	../common/musl-cc -O2 -Wall -Wextra -Werror -I../../../include main.c report.c -o $@
clean:
	rm -f ../apps-bin/$(TARGET)
