# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# libc.mk - building programs against picolibc.
#
# lib/program.mk builds against lib/ulib, this system's own few hundred
# lines of wrappers. This builds against a real C library -- stdio,
# printf, malloc, setjmp, the lot -- as installed by libc/build.sh. A
# Makefile sets TOPDIR and PROGS and includes this.
#
# The headers are picolibc's, not uapi.h: a program written for a POSIX
# system is written against <stdio.h> and <unistd.h>, and gets them.

M68K_PREFIX ?= $(if $(wildcard $(HOME)/m68k/install/bin/m68k-elf-gcc),$(HOME)/m68k/install/bin/,)
SAGE_LIBC   ?= $(HOME)/m68k/sage040-libc

LIBC_DIR := $(TOPDIR)/libc

CC   := $(M68K_PREFIX)m68k-elf-gcc
SIZE := $(M68K_PREFIX)m68k-elf-size

CFLAGS  := -mcpu=68040 -O2 -Wall -Wextra -nostdinc -nostdlib \
           -isystem $(SAGE_LIBC)/include \
           -isystem $(shell $(CC) -print-file-name=include) \
           -ffunction-sections -fdata-sections $(EXTRA_CFLAGS)
LDFLAGS := -mcpu=68040 -nostdlib -T $(LIBC_DIR)/sage040.ld \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,--gc-sections
LDLIBS  := -L$(SAGE_LIBC)/lib -Wl,--start-group -lc -llinux -Wl,--end-group \
           -lgcc

LIBC_DEPS := $(LIBC_DIR)/crt0.s $(LIBC_DIR)/sage040.ld \
             $(SAGE_LIBC)/lib/libc.a $(SAGE_LIBC)/lib/liblinux.a

.PHONY: all install clean

all: $(PROGS)

%: %.c $(LIBC_DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LIBC_DIR)/crt0.s $< $(LDLIBS) -o $@
	@$(SIZE) $@

clean:
	rm -f $(PROGS)
