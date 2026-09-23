# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# binutils, native: fetched and built by build.sh into ~/m68k/src.
# Nothing built lands in this directory.
#
# These go in /usr/bin, not /BIN. /BIN is the machine's own small
# programs and the ported utilities; /usr/bin is where a toolchain
# installed under --prefix=/usr puts itself, and where the native gcc
# will look for `as` and `ld` when it runs them. Keeping that true
# means gcc needs no -B to find its own assembler.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-binutils-native/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /usr
	@$(FSIMG) mkdir /usr/bin
	@for b in $(OUT)/bin/*; do \
	    $(FSIMG) put -m 755 $$b /usr/bin/$$(basename $$b); \
	done
	@echo "binutils is on $(DISK) at /usr/bin"

clean:
	rm -rf $(OUT)
