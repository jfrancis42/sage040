# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# GNU sed: fetched, built by build.sh; installed as /bin/SED.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 sed /bin/sed

clean:
	rm -f sed
