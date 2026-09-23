# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# The C library, and its installation ON THE MACHINE.
#
# `make -C libc` builds picolibc for this system into $SAGE_LIBC on the
# host, which is what every cross build here links against. `make -C
# libc install` is a different thing: it puts the C library on the
# machine's own disk, under /usr, where a compiler RUNNING ON THE
# MACHINE will look for it.
#
# Nothing needed that until now -- every program was cross-compiled, so
# the headers only ever had to exist on the host. The native toolchain
# (task 49) is the first thing that has to find <stdio.h> at
# /usr/include and libc.a at /usr/lib, because it is running here.
#
# WHAT GOES ON, and why each piece:
#
#   /usr/include    picolibc's headers, and the ones libc/net adds
#   /usr/lib        libc.a, libc.so, liblinux.a, libm.a -- and
#                   libgcc.a, which is not part of the C library at
#                   all but is the compiler's own runtime (64-bit
#                   division, floating point helpers) and has to be
#                   beside it or nothing links
#   /usr/lib/crt0.o, crt0-dyn.o
#                   where a program starts. They are assembly source
#                   in this tree and objects on the machine: the
#                   native gcc's specs name them by filename
#   /usr/lib/sage040.ld
#                   the linker script a STATIC program is laid out by
#
# /lib keeps ld.so and libc.so, which is where the loader looks at run
# time; /usr/lib is where the LINKER looks at build time. They are the
# same libc.so, installed twice, for the same reason every Unix does it.

TOPDIR := ..
include $(TOPDIR)/disk.mk

SAGE_LIBC ?= $(HOME)/m68k/sage040-libc
M68K_PREFIX ?= $(if $(wildcard $(HOME)/m68k/install/bin/m68k-elf-gcc),$(HOME)/m68k/install/bin/,)
CC := $(M68K_PREFIX)m68k-elf-gcc

.DEFAULT_GOAL := all
.PHONY: all install install-usr clean

all:
	./build.sh

# The start files as OBJECTS. On the host they are assembled into each
# program from source; on the machine the compiler needs a file it can
# name in a spec, so they are assembled once here.
crt0.o: crt0.s
	$(CC) -mcpu=68040 -c $< -o $@

crt0-dyn.o: crt0-dyn.s
	$(CC) -mcpu=68040 -c $< -o $@

install: install-usr

install-usr: all crt0.o crt0-dyn.o $(DISK)
	@mmd -D s -i $(MIMG) ::/usr 2>/dev/null || true
	@mmd -D s -i $(MIMG) ::/usr/include 2>/dev/null || true
	@mmd -D s -i $(MIMG) ::/usr/lib 2>/dev/null || true
	mcopy -s -o -i $(MIMG) $(SAGE_LIBC)/include/* ::/usr/include/
	mcopy -o -i $(MIMG) $(SAGE_LIBC)/lib/libc.a ::/usr/lib/
	mcopy -o -i $(MIMG) $(SAGE_LIBC)/lib/libc.so ::/usr/lib/
	mcopy -o -i $(MIMG) $(SAGE_LIBC)/lib/liblinux.a ::/usr/lib/
	mcopy -o -i $(MIMG) $(SAGE_LIBC)/lib/libm.a ::/usr/lib/
	mcopy -o -i $(MIMG) $(SAGE_LIBC)/lib/libtermcap.a ::/usr/lib/
	mcopy -o -i $(MIMG) $(shell $(CC) -mcpu=68040 -print-libgcc-file-name) ::/usr/lib/
	mcopy -o -i $(MIMG) crt0.o crt0-dyn.o ::/usr/lib/
	mcopy -o -i $(MIMG) sage040.ld ::/usr/lib/
	@echo "the C library is on $(DISK) at /usr"

clean:
	rm -f crt0.o crt0-dyn.o
