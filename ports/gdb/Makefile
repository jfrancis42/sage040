# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# gdb: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# What goes on the machine: /usr/bin/gdb and /usr/bin/gdbserver.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-gdb-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean test

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /usr
	@$(FSIMG) mkdir /usr/bin
	$(FSIMG) put -m 755 $(OUT)/bin/gdb /usr/bin/gdb
	$(FSIMG) put -m 755 $(OUT)/bin/gdbserver /usr/bin/gdbserver
	@echo "gdb and gdbserver are in /usr/bin on $(DISK)"

test:
	cd $(TOPDIR)/kernel && ./gdbtest.sh

clean:
	rm -rf $(SRCDIR)/build-gdb-sage040
