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
READELF := $(M68K_PREFIX)m68k-elf-readelf

CFLAGS  := -mcpu=68040 -O2 -Wall -Wextra -nostdinc -nostdlib \
           -isystem $(SAGE_LIBC)/include \
           -isystem $(shell $(CC) -print-file-name=include) \
           -ffunction-sections -fdata-sections $(EXTRA_CFLAGS)
#
# LINK=dynamic links against /lib/libc.so, through /lib/ld.so; the
# default is a static program, which runs on a disk with neither. The
# static link says -Bstatic, because with libc.so installed beside
# libc.a, a plain -lc finds the shared one -- and says it to the LINKER:
# this bare-metal gcc's driver does not pass -static on, so -static
# alone produced a dynamic program asking for /usr/lib/libc.so.1.
#
# A dynamic program is linked by GNU ld's own script, at 0x10000000 like
# every other program: it needs .interp, .dynamic, .got and .plt, which
# sage040.ld does not describe, and the stock script brackets the
# constructor arrays with the symbols crt0-dyn.s walks. -z now because
# ld.so binds eagerly anyway, and sysv hashes because that is what
# ld.so reads.
LINK ?= static

DYN_LDFLAGS := -mcpu=68040 -nostdlib -Wl,-Ttext-segment=0x10000000 \
               -Wl,--dynamic-linker=/lib/ld.so -Wl,-z,now -Wl,--hash-style=sysv \
               -Wl,--build-id=none -Wl,--gc-sections
DYN_LDLIBS  := -L$(SAGE_LIBC)/lib -lc -lgcc
DYN_DEPS    := $(LIBC_DIR)/crt0-dyn.s $(SAGE_LIBC)/lib/libc.so

STATIC_LDFLAGS := -mcpu=68040 -nostdlib -Wl,-Bstatic -T $(LIBC_DIR)/sage040.ld \
                  -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,--gc-sections
STATIC_LDLIBS  := -L$(SAGE_LIBC)/lib -Wl,--start-group -lc -llinux \
                  -Wl,--end-group -lgcc
STATIC_DEPS    := $(LIBC_DIR)/crt0.s $(LIBC_DIR)/sage040.ld \
                  $(SAGE_LIBC)/lib/libc.a $(SAGE_LIBC)/lib/liblinux.a

ifeq ($(LINK),dynamic)
CRT0      := $(LIBC_DIR)/crt0-dyn.s
LDFLAGS   := $(DYN_LDFLAGS)
LDLIBS    := $(DYN_LDLIBS)
LIBC_DEPS := $(DYN_DEPS)
else
CRT0      := $(LIBC_DIR)/crt0.s
LDFLAGS   := $(STATIC_LDFLAGS)
LDLIBS    := $(STATIC_LDLIBS)
LIBC_DEPS := $(STATIC_DEPS)
endif

.PHONY: all install clean

all: $(PROGS)

%: %.c $(LIBC_DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(CRT0) $< $(LDLIBS) -o $@
ifneq ($(LINK),dynamic)
	@if $(READELF) -l $@ | grep -q INTERP; then \
	    echo "libc.mk: $@ came out dynamic from a static link" >&2; \
	    rm -f $@; exit 1; fi
endif
	@$(SIZE) $@

# NAME.dyn: the same program linked against libc.so whatever LINK says,
# so one tree can hold both and a test can run the pair.
%.dyn: %.c $(DYN_DEPS)
	$(CC) $(CFLAGS) $(DYN_LDFLAGS) $(LIBC_DIR)/crt0-dyn.s $< $(DYN_LDLIBS) -o $@
	@$(SIZE) $@

clean:
	rm -f $(PROGS)
