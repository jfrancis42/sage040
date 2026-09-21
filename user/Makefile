# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Programs for the Sage040.
#
#   make            build every program
#   make install    copy them onto the machine's disk
#   make list       what is on the disk now
#
# A program is an ordinary ELF executable linked at 1 MB. There is no
# flattening step: the kernel reads the program headers, which is why it
# can put each segment where the linker said and zero the rest.
#
# Programs carry NO EXTENSION. The kernel decides what is executable by
# looking at the first four bytes of the file, not at its name -- a FAT16
# volume has no execute permission bit to consult, and Unix has always
# used the format rather than the name anyway.

M68K_PREFIX ?= $(HOME)/m68k/install
TOOLCHAIN := $(if $(wildcard $(M68K_PREFIX)/bin/m68k-elf-gcc),$(M68K_PREFIX)/bin/m68k-elf,m68k-elf)

CC      := $(TOOLCHAIN)-gcc
SIZE    := $(TOOLCHAIN)-size
OBJDUMP := $(TOOLCHAIN)-objdump
READELF := $(TOOLCHAIN)-readelf

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

# uapi.h is the system call ABI and the only kernel header a program
# gets. sage040.h is hardware, and only cube.c has any business with it.
CPUFLAGS := -mcpu=68040
CFLAGS   := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
            -Wall -Wextra -Werror -fno-builtin -fno-stack-protector \
            -I. -I../kernel -I../tests
LDFLAGS  := $(CPUFLAGS) -ffreestanding -nostdlib -T user.ld \
            -Wl,--build-id=none -Wl,--no-warn-rwx-segments

PROGS := cube hello
COMMON := crt0.s ulib.c ulib.h user.ld

.PHONY: all install list disasm clean

all: $(PROGS)

# The name of the program is the name of the file, with no suffix on
# either side. `cube` on the disk is CUBE.
%: %.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) \
	    -x assembler-with-cpp crt0.s \
	    -x c ulib.c $< -o $@
	@$(SIZE) $@

install: $(PROGS) $(DISK)
	@for p in $(PROGS); do \
	   mcopy -o -i $(MIMG) $$p ::/$$(echo $$p | tr a-z A-Z); \
	   echo "$$p -> $(DISK) as $$(echo $$p | tr a-z A-Z) ($$(stat -c%s $$p) bytes)"; \
	 done

list: $(DISK)
	@mdir -i $(MIMG) ::/

disasm-%: %
	$(OBJDUMP) -d $<

headers-%: %
	$(READELF) -lh $<

clean:
	rm -f $(PROGS)
