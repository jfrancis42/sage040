# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# libstdcxx: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# A LIBRARY -- nothing to put on the machine's disk, because what
# links against it is built here. the C++ standard library. GCC and GDB are both written in C++, so a compiler that runs on the machine needs one that runs on the machine.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-libstdcxx-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all
	@echo "libstdcxx: $(OUT)/lib -- a library; nothing to put on the disk"

clean:
	rm -rf $(OUT)
