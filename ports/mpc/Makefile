# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# mpc: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# A LIBRARY -- nothing to put on the machine's disk, because what
# links against it is built here. the same, for complex constants.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-mpc-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all
	@echo "mpc: $(OUT)/lib -- a library; nothing to put on the disk"

clean:
	rm -rf $(OUT)
