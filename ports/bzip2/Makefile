# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# bzip2: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# The LIBRARY is not installed on the machine and does not need to be:
# it is linked into the programs that use it, here on the host. What
# goes on the disk is what somebody can run.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-bzip2-sage040/sage040
BINS   := bzip2

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	@for b in $(BINS); do \
	    if [ -x $(OUT)/bin/$$b ]; then \
	        $(FSIMG) put -m 755 $(OUT)/bin/$$b /bin/$$b; \
	    fi; \
	done
	@echo "bzip2 is on $(DISK)"

clean:
	rm -rf $(OUT)
