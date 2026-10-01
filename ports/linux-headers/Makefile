# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Linux's m68k userspace headers, for programs built against the kernel
# interface directly (ports/musl, ports/strace). See build.sh.
#
# Nothing goes on the machine: what is built here is linked into, or
# compiled into, the programs that use it.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all

clean:
	rm -rf $(HOME)/m68k/src/build-linux-headers-sage040
