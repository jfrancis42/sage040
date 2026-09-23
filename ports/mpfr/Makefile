# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# mpfr: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# A LIBRARY -- nothing to put on the machine's disk, because what
# links against it is built here. gcc folds floating-point constants in it, to the answer the TARGET would give.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-mpfr-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all
	@echo "mpfr: $(OUT)/lib -- a library; nothing to put on the disk"

clean:
	rm -rf $(OUT)
