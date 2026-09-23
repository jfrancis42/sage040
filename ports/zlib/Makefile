# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# zlib: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# A LIBRARY ONLY -- there is no program to put on the disk. zlib's own
# minigzip is a demonstration, not a tool; gzip proper would come from
# sbase. What links against this is CPython (zlib, gzip, zipfile, and
# so every wheel pip unpacks).

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-zlib-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all
	@echo "zlib: $(OUT)/lib -- a library; nothing to put on the disk"

clean:
	rm -rf $(OUT)
