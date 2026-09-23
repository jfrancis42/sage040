# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# rsync: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# It needs ssh on the machine to be useful over the network, which is
# ports/dropbear -- rsync runs `ssh host rsync --server` and talks its
# protocol down the pipe. It works on local paths with no network at
# all, which is how the test exercises it.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-rsync-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@mmd -D s -i $(MIMG) ::/BIN 2>/dev/null || true
	mcopy -o -i $(MIMG) $(OUT)/bin/rsync ::/BIN/rsync
	@echo "rsync is on $(DISK)"

clean:
	rm -rf $(OUT)
