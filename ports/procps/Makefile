# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# procps-ng: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# Installed: ps, top, free, pgrep, pkill, pidof, pmap, pwdx, vmstat,
# watch, tload. NOT installed: w and uptime, which the system has its own
# of (system/w.c, system/uptime.c), and kill, which sbase's is; sysctl and
# slabtop, which read files this kernel does not have (/proc/sys,
# /proc/slabinfo). All are built, and kernel/procpstest.sh tries w,
# uptime and kill as well.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
POUT   := $(SRCDIR)/build-procps-sage040/sage040
PROGS  := ps top free pgrep pkill pidof pmap pwdx vmstat watch tload

.DEFAULT_GOAL := all
.PHONY: all install clean test

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	@for p in $(PROGS); do $(FSIMG) put -m 755 $(POUT)/bin/$$p /bin/$$p; done
	@echo "procps-ng ($(PROGS)) is on $(DISK)"

test:
	cd $(TOPDIR)/kernel && ./procpstest.sh

clean:
	rm -rf $(POUT)
