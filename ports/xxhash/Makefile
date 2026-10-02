# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# xxHash: fetched and built by build.sh into ~/m68k/src. The library is
# linked into rsync; xxhsum goes on the disk.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
XOUT   := $(SRCDIR)/build-xxhash-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean test

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(XOUT)/bin/xxhsum /bin/xxhsum
	@echo "xxhsum is on $(DISK)"

test:
	cd $(TOPDIR)/kernel && ./xxhtest.sh

clean:
	rm -rf $(XOUT)
