# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# GNU diffutils: fetched and built by build.sh into ~/m68k/src. Nothing
# built lands in this directory.
#
# diff, diff3 and sdiff go to /usr/bin. So does GNU cmp, where sbase's
# /bin/cmp, earlier in PATH, still answers to plain `cmp`.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-diffutils-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /usr
	@$(FSIMG) mkdir /usr/bin
	@for p in diff cmp diff3 sdiff; do \
	    $(FSIMG) put -m 755 $(OUT)/bin/$$p /usr/bin/$$p || exit 1; \
	 done
	@echo "diff, cmp, diff3 and sdiff are in /usr/bin on $(DISK)"

clean:
	rm -rf $(OUT)
