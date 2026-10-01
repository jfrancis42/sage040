# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# GNU patch: fetched and built by build.sh into ~/m68k/src. Nothing
# built lands in this directory.
#
# patch goes to /usr/bin.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-patch-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /usr
	@$(FSIMG) mkdir /usr/bin
	@for p in patch; do \
	    $(FSIMG) put -m 755 $(OUT)/bin/$$p /usr/bin/$$p || exit 1; \
	 done
	@echo "patch is in /usr/bin on $(DISK)"

clean:
	rm -rf $(OUT)
