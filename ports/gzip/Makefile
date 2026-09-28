# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# gzip: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# One program, installed as gzip, gunzip and zcat: it reads its own
# name to decide which it is.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-gzip-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	@for n in gzip gunzip zcat; do \
	    $(FSIMG) put -m 755 $(OUT)/bin/gzip /bin/$$n || exit 1; \
	done
	@echo "gzip, gunzip and zcat are on $(DISK)"

clean:
	rm -rf $(OUT)
