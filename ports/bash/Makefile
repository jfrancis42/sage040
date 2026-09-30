# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# GNU bash: fetched, built by build.sh; installed as /bin/BASH.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 bash /bin/bash
	@# /bin/sh is bash, as on most Linux systems: make, ./configure,
	@# system() and popen() all run /bin/sh and all expect POSIX sh.
	@# Run as sh, bash is in its POSIX mode. The system's own shell is
	@# /bin/msh (system/Makefile).
	$(FSIMG) symlink bash /bin/sh

clean:
	rm -f bash
