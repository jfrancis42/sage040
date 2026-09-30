# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# GNU make: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# It goes to /bin/make, over sbase's POSIX make, which cannot read the
# Makefiles Perl's MakeMaker and automake write. Installed after sbase
# for that reason (the top-level Makefile's port order).

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-make-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(OUT)/bin/make /bin/make
	@echo "GNU make is /bin/make on $(DISK)"

clean:
	rm -rf $(OUT)
