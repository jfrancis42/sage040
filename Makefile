# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Sage040 - top level.
#
# Each subdirectory builds on its own; this is the shortest path to the
# common things.
#
#   make            build the boot ROM and the kernel
#   make boot       put the kernel on the disk and boot the machine
#   make test       device tests, then the kernel filesystem test
#   make disk       create the disk image if it is not there
#   make disk-ls    partition table and directory listing
#   make disk-fsck  check the filesystem with the host's tools
#   make clean      remove build artifacts, keeping the disk
#   make distclean  also remove the disk image

TOPDIR := .
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

.PHONY: all boot run test tests fstest cube clean distclean

all:
	$(MAKE) -C bootrom
	$(MAKE) -C kernel

# The machine as it is meant to run: the ROM loads KERNEL.ROM off the
# filesystem and jumps to it.
boot:
	$(MAKE) -C kernel boot

# The kernel without the boot ROM in the way. Same kernel, quicker loop.
run:
	$(MAKE) -C kernel run

test: tests fstest

tests:
	$(MAKE) -C tests run

fstest:
	cd kernel && ./fstest.sh

cube:
	$(MAKE) -C cube

clean:
	$(MAKE) -C bootrom clean
	$(MAKE) -C kernel clean
	$(MAKE) -C cube clean
	$(MAKE) -C tests clean
	rm -f kernel/hd-test.img kernel/fstest.log

distclean: clean disk-clean
