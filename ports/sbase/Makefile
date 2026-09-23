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
	$(FSIMG) mkdir /bin
	$(FSIMG) mkdir /share
	$(FSIMG) mkdir /share/misc
	$(FSIMG) put -m 755 bin/* /bin/
	$(FSIMG) put share/misc/bc.library /share/misc/

clean:
	rm -rf bin share
