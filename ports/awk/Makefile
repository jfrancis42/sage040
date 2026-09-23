# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# The one true awk: fetched, built by build.sh; installed as /bin/AWK.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 awk /bin/awk

clean:
	rm -f awk
