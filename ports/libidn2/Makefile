# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# libidn2: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# The library is linked into curl, wget and libpsl on the host. What
# goes on the disk is `idn2`, which converts a name to and from the
# form DNS carries: `idn2 bücher.example` prints xn--bcher-kva.example.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-libidn2-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(OUT)/bin/idn2 /bin/idn2
	@echo "libidn2 is on $(DISK)"

clean:
	rm -rf $(OUT)
