# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# PCRE2: fetched and built by build.sh into ~/m68k/src. The library is
# linked into grep (grep -P); pcre2grep goes on the disk.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
POUT   := $(SRCDIR)/build-pcre2-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean test

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(POUT)/bin/pcre2grep /bin/pcre2grep
	@echo "pcre2grep is on $(DISK)"

test:
	cd $(TOPDIR)/kernel && ./pcretest.sh

clean:
	rm -rf $(POUT)
