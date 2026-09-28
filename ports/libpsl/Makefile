# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# libpsl: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# The library, with the list compiled in, is linked into curl and wget.
# What goes on the disk is `psl`, which answers questions about the
# list: `psl --print-unreg-domain www.example.co.uk` prints co.uk.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-libpsl-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(OUT)/bin/psl /bin/psl
	@echo "libpsl is on $(DISK)"

clean:
	rm -rf $(OUT)
