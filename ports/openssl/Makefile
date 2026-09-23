# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# openssl: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# The LIBRARY is not installed on the machine and does not need to be:
# it is linked into the programs that use it, here on the host. What
# goes on the disk is what somebody can run.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-openssl-sage040/sage040
BINS   := openssl

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@mmd -D s -i $(MIMG) ::/BIN 2>/dev/null || true
	@for b in $(BINS); do \
	    if [ -x $(OUT)/bin/$$b ]; then \
	        mcopy -o -i $(MIMG) $(OUT)/bin/$$b ::/BIN/$$b; \
	    fi; \
	done
	@echo "openssl is on $(DISK)"

clean:
	rm -rf $(OUT)
