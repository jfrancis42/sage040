# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# sbase: cloned, built by build.sh into ./bin; installed into /BIN, with
# bc's math library at /share/misc/bc.library.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	mmd -D s -i $(MIMG) ::/BIN ::/share ::/share/misc 2>/dev/null || true
	mcopy -o -i $(MIMG) bin/* ::/BIN/
	mcopy -o -i $(MIMG) share/misc/bc.library ::/share/misc/

clean:
	rm -rf bin share
