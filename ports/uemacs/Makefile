# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# uEmacs/PK: fetched, patched and built by build.sh; installed as /BIN/EM.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	mmd -D s -i $(MIMG) ::/BIN 2>/dev/null || true
	mcopy -o -i $(MIMG) em ::/BIN/EM

clean:
	rm -f em
