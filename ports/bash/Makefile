# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# GNU bash: fetched, built by build.sh; installed as /BIN/BASH.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	mmd -D s -i $(MIMG) ::/BIN 2>/dev/null || true
	mcopy -o -i $(MIMG) bash ::/BIN/BASH

clean:
	rm -f bash
