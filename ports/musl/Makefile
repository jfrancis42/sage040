# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# musl: a second C library, static, for the programs that need Linux's
# constants as they are (ports/strace). See build.sh.
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
	rm -rf $(HOME)/m68k/src/build-musl-sage040
