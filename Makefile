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

.PHONY: all boot run world ports pylibs python etc test tests cryptotest fstest edittest vmtest nettest apitest vttest libctest fscktest uemacstest vitest dnstest tcptest sotest pagetest devtest lotest awktest sedtest greptest sbasetest bashtest bashsuite threadtest curstest logtest lesstest crontest ptytest pytest pylibtest dftest usertest libc cube programs clean distclean

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
	$(MAKE) -C ldso install

# EVERYTHING PORTED, onto the machine's disk.
#
# A port installs only when told to, because each one fetches and builds
# its own source and that takes time somebody may not want spent. The
# consequence, which cost a boot with an empty-looking /bin: `make boot`
# puts the SYSTEM on the disk and nothing else, and the test suites build
# scratch disks of their own -- so a program can be proven working and
# still not be anywhere you can run it.
#
# This is the one command that puts the lot on hd.img. Each port builds
# first if it has not been built.
ports:
	$(MAKE) -C ports/sbase install
	$(MAKE) -C ports/awk install
	$(MAKE) -C ports/sed install
	$(MAKE) -C ports/grep install
	$(MAKE) -C ports/bash install
	$(MAKE) -C ports/ncurses install
	$(MAKE) -C ports/less install
	$(MAKE) -C ports/uemacs install
	$(MAKE) -C ports/vi install
	$(MAKE) -C ports/bzip2 install
	$(MAKE) -C ports/xz install
	$(MAKE) -C ports/zstd install
	$(MAKE) -C ports/sqlite install
	$(MAKE) -C ports/openssl install
	$(MAKE) -C ports/readline install
	$(MAKE) -C ports/libffi install
	$(MAKE) -C ports/dropbear install
	$(MAKE) -C ports/rsync install
	@echo
	@echo "Python is not in the list above: it is 45 MB and 2,244 files,"
	@echo "and copying it takes minutes. 'make python' installs it."

# WHAT CPYTHON IS BUILT AGAINST. Each of these is a standard library
# module that exists or does not depending on whether its library was
# there when Python was configured -- so they have to be built BEFORE
# python, and rebuilding one means rebuilding python to pick it up.
#
#   zlib      zlib, gzip, zipfile, and pip's wheels
#   bzip2     bz2
#   xz        lzma
#   zstd      compression.zstd, new in 3.14
#   sqlite    sqlite3
#   openssl   ssl, and a hashlib whose digests come from OpenSSL
#             instead of the bundled HACL* code (which gets MD5 wrong
#             on a big-endian machine -- see ports/python/patches/04)
#   readline  line editing and history at the interactive prompt
#   libffi    ctypes, as far as it goes without dlopen
#   ncurses   curses
pylibs:
	$(MAKE) -C ports/zlib install
	$(MAKE) -C ports/bzip2 install
	$(MAKE) -C ports/xz install
	$(MAKE) -C ports/zstd install
	$(MAKE) -C ports/sqlite install
	$(MAKE) -C ports/openssl install
	$(MAKE) -C ports/readline install
	$(MAKE) -C ports/libffi install
	$(MAKE) -C ports/dropbear install
	$(MAKE) -C ports/rsync install
	$(MAKE) -C ports/ncurses install

python: pylibs
	$(MAKE) -C ports/python install

# /etc/rc, replaced whatever it says. `make programs` installs it only
# when the disk has none or still has the unedited default.
etc:
	$(MAKE) -C system etc

# The whole machine: the system, every port, and Python.
world: programs ports python

# The kernel without the boot ROM in the way. Same kernel, quicker loop.
run:
	$(MAKE) -C kernel run

test: tests cryptotest fstest apitest edittest vmtest nettest vttest libctest fscktest uemacstest vitest dnstest tcptest sotest pagetest devtest lotest awktest sedtest greptest sbasetest bashtest threadtest curstest logtest lesstest crontest ptytest pytest pylibtest dftest usertest

tests:
	$(MAKE) -C tests run

cryptotest:
	cd kernel && ./cryptotest.sh

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

dnstest:
	cd kernel && ./dnstest.sh

tcptest:
	cd kernel && ./tcptest.sh

sotest:
	cd kernel && ./sotest.sh

pagetest:
	cd kernel && ./pagetest.sh

devtest:
	cd kernel && ./devtest.sh

lotest:
	cd kernel && ./lotest.sh

awktest:
	cd kernel && ./awktest.sh

sedtest:
	cd kernel && ./sedtest.sh

greptest:
	cd kernel && ./greptest.sh

sbasetest:
	cd kernel && ./sbasetest.sh

bashtest:
	cd kernel && ./bashtest.sh

# Threads: clone(2), futexes and the pthread layer over them.
threadtest:
	cd kernel && ./threadtest.sh

# terminfo and curses (ncurses), with the database renamed away as the
# control.
curstest:
	cd kernel && ./curstest.sh

# The kernel's log, /dev/klog, klogd and /var/log/syslog.
logtest:
	cd kernel && ./logtest.sh

# less: a full-screen program on this terminal, and on one that cannot
# address its cursor.
lesstest:
	cd kernel && ./lesstest.sh

# cron: something the machine does by itself, later. Takes three
# minutes of real time, because a minute-resolution cron cannot be
# hurried.
crontest:
	cd kernel && ./crontest.sh

# Pseudo-terminals: /dev/ptmx, /dev/pts/N, and a terminal with a program
# at each end.
ptytest:
	cd kernel && ./ptytest.sh

# CPython: the interpreter, the standard library off the disk, and every
# answer compared with the host's Python.
pytest:
	cd kernel && ./pytest.sh

# The libraries CPython is built against, tested through their own
# programs: every stream crosses the host boundary in both directions.
pylibtest:
	cd kernel && ./pylibtest.sh

# df and du: the numbers checked against the host's own view of the
# same disk image, and the shell's built-in df as the negative control.
dftest:
	cd kernel && ./dftest.sh

# Users, /etc/passwd and home directories -- and a check that the
# absence of file ownership is honest rather than accidental.
usertest:
	cd kernel && ./usertest.sh

# Every one of bash's own 83 tests, not the subset: hours, not minutes.
bashsuite:
	cd kernel && BASH_TESTS=all ./bashtest.sh

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
	$(MAKE) -C ldso clean
	$(MAKE) -C tests clean
	#
	# Everything the test suites write lives in scratch/ -- the disk
	# images most of all, which are 16 MB each and used to sit beside
	# the source with names that looked like part of it. hd.img is NOT
	# in there: that is the machine's own disk, not a build product.
	#
	rm -rf scratch

distclean: clean disk-clean
