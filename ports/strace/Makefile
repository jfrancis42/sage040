# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# strace: fetched and built by build.sh into ~/m68k/src, statically
# with musl (ports/musl). Nothing built lands in this directory.
#
# What goes on the machine: /usr/bin/strace, one static program.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-strace-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean test

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /usr
	@$(FSIMG) mkdir /usr/bin
	$(FSIMG) put -m 755 $(OUT)/bin/strace /usr/bin/strace
	@echo "strace is in /usr/bin on $(DISK)"

test:
	cd $(TOPDIR)/kernel && ./stracetest.sh

clean:
	rm -rf $(OUT)
