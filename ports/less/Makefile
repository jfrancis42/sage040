# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# less: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
LOUT   := $(SRCDIR)/build-less-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean test

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(LOUT)/bin/less /bin/less
	@[ -x $(LOUT)/bin/lesskey ] && \
	    $(FSIMG) put -m 755 $(LOUT)/bin/lesskey /bin/lesskey || true
	@echo "less is on $(DISK)"

test:
	cd $(TOPDIR)/kernel && ./lesstest.sh

clean:
	rm -rf $(LOUT)
