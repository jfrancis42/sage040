# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Small utilities built against PICOLIBC, where system/ builds against
# lib/ulib: what they do is the C library's to answer (locale(1) is
# setlocale and nl_langinfo, and nothing else).

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := locale

include $(TOPDIR)/libc/libc.mk

install: $(PROGS) $(DISK)
	@$(FSIMG) mkdir /bin
	@for p in $(PROGS); do $(FSIMG) put -m 755 $$p /bin/$$p; done
	@echo "$(PROGS) -> /bin"
