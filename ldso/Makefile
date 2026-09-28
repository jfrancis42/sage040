# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# The dynamic linker, /lib/ld.so. Freestanding: no C library, since the
# C library is one of the things it loads.

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

SAGE_LIBC ?= $(HOME)/m68k/sage040-libc
# The toolchain's INSTALL ROOT, as everywhere else (see libc/libc.mk).
M68K_PREFIX ?= $(HOME)/m68k/install
TOOLCHAIN := $(if $(wildcard $(M68K_PREFIX)/bin/m68k-elf-gcc),$(M68K_PREFIX)/bin/m68k-elf,m68k-elf)
CC   := $(TOOLCHAIN)-gcc
SIZE := $(TOOLCHAIN)-size

CFLAGS  := -mcpu=68040 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
           -nostdlib -fno-pic
LDFLAGS := -nostdlib -T ld.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments

.PHONY: all install clean
all: ld.so

# /lib/ld.so, and /lib/libc.so beside it when picolibc has been built
# (`make libc`): what every dynamically linked program on the disk needs.
install: ld.so $(DISK)
	@$(FSIMG) mkdir /lib
	@$(FSIMG) put -m 755 ld.so /lib/ld.so
	@if [ -f $(SAGE_LIBC)/lib/libc.so ]; then \
	    $(FSIMG) put -m 755 $(SAGE_LIBC)/lib/libc.so /lib/libc.so; \
	    echo "installed /lib/ld.so and /lib/libc.so"; \
	else echo "installed /lib/ld.so; no libc.so -- run 'make libc'"; fi

ld.so: start.s ld.c ld.ld
	$(CC) $(CFLAGS) $(LDFLAGS) start.s ld.c -lgcc -o $@
	@$(SIZE) $@

clean:
	rm -f ld.so
