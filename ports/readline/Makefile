# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# readline: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# A LIBRARY ONLY -- there is no program to put on the disk. It is what
# CPython's interactive prompt links against for line editing, history
# and completion.
# So `install` builds it and says where it is; what ends up on the
# machine is whatever links against it.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-readline-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all
	@echo "readline: $(OUT)/lib -- a library; nothing to put on the disk"

clean:
	rm -rf $(OUT)
