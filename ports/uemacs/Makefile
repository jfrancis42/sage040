# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# uEmacs/PK: fetched, patched and built by build.sh; installed as /bin/EM.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 em /bin/em

clean:
	rm -f em
