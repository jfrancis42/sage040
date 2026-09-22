# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Sage040 - top level.
#
# Each subdirectory builds on its own; this is the shortest path to the
# common things.
#
#   make            build the boot ROM and the kernel
#   make boot       put the kernel and programs on the disk, and boot
#   make test       device tests, then the kernel filesystem test
#   make libc       build picolibc for programs (needed by libctest)
#   make disk       create the disk image if it is not there
#   make disk-ls    partition table and directory listing
#   make disk-fsck  check the filesystem with the host's tools
#   make clean      remove build artifacts, keeping the disk
#   make distclean  also remove the disk image

TOPDIR := .
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

.PHONY: all boot run test tests fstest edittest vmtest nettest apitest vttest libctest fscktest uemacstest vitest libc cube programs clean distclean

all:
	$(MAKE) -C bootrom
	$(MAKE) -C kernel
	$(MAKE) -C system
	$(MAKE) -C apps

# The machine as it is meant to run: the ROM loads KERNEL.ROM off the
# filesystem and jumps to it.
boot: programs
	$(MAKE) -C kernel boot

#
# The programs that live on the disk alongside the kernel.
#
# Two sets, and the split is deliberate. system/ is the machine's own --
# ifconfig, ping, netstat, shutdown, env -- and installs into /bin, which
# is where PATH looks first. apps/ is everything somebody chose to run,
# and installs in the root, which PATH reaches last through "." -- so an
# application cannot quietly stand in for a system program of the same
# name.
#
# Neither is privileged. Being part of the system buys a program nothing
# except a place on a fresh disk.
#
programs:
	$(MAKE) -C system install
	$(MAKE) -C apps install

# The kernel without the boot ROM in the way. Same kernel, quicker loop.
run:
	$(MAKE) -C kernel run

test: tests fstest apitest edittest vmtest nettest vttest libctest fscktest uemacstest vitest

tests:
	$(MAKE) -C tests run

fstest:
	cd kernel && ./fstest.sh

edittest:
	cd kernel && ./edittest.sh

vmtest:
	cd kernel && ./vmtest.sh

nettest:
	cd kernel && ./nettest.sh

vttest:
	cd kernel && ./vttest.sh

libctest:
	cd kernel && ./libctest.sh

fscktest:
	cd kernel && ./fscktest.sh

uemacstest:
	cd kernel && ./uemacstest.sh

vitest:
	cd kernel && ./vitest.sh

# picolibc, built and installed outside the tree (libc/README.md). Once.
libc:
	libc/build.sh

# The system call surface a ported program expects. Grows with the
# porting work; see progress.md.
apitest:
	cd kernel && ./apitest.sh

cube:
	$(MAKE) -C cube

clean:
	$(MAKE) -C bootrom clean
	$(MAKE) -C kernel clean
	$(MAKE) -C cube clean
	$(MAKE) -C system clean
	$(MAKE) -C apps clean
	$(MAKE) -C tests clean
	#
	# Everything the test suites write lives in scratch/ -- the disk
	# images most of all, which are 16 MB each and used to sit beside
	# the source with names that looked like part of it. hd.img is NOT
	# in there: that is the machine's own disk, not a build product.
	#
	rm -rf scratch

distclean: clean disk-clean
