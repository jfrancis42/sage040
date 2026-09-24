# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Sage040 - top level.
#
# Each subdirectory builds on its own; this is the shortest path to the
# common things.
#
#   make            build the boot ROM and the kernel
#   make install    put EVERYTHING on the disk: the system, every port,
#                   Python, the native toolchain, and the kernel source
#   make copy-src   put the kernel's own source on the disk, so the
#                   machine can rebuild its own kernel
#   make qemu       build and install the patched emulator itself
#   make boot       install everything, then boot it
#   make programs   the system's own programs only -- the short way round
#                   when iterating on one of them
#   make test       every suite in the tree (see the `test:` target)
#   make libc       build picolibc for programs (needed by libctest)
#   make disk       create the disk image if it is not there
#   make disk-ls    partition table and directory listing
#   make disk-fsck  check the filesystem with the host's tools
#   make clean      remove build artifacts, keeping the disk
#   make distclean  also remove the disk image

TOPDIR := .
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

.PHONY: logintest fsimgtest fattest all boot run install src qemu world libc-if-missing toolchain ports pylibs python etc test tests cryptotest fstest edittest vmtest nettest apitest vttest libctest fscktest uemacstest vitest dnstest tcptest sotest pagetest devtest lotest awktest sedtest greptest sbasetest bashtest bashsuite threadtest curstest logtest lesstest crontest ptytest pytest pylibtest dftest usertest linktest sshtest whotest nativetest qemutest libc cube programs clean distclean

all:
	$(MAKE) -C bootrom
	$(MAKE) -C kernel
	$(MAKE) -C system
	$(MAKE) -C apps
	$(MAKE) -C auth

# The machine as it is meant to run: the ROM loads KERNEL.ROM off the
# filesystem and jumps to it.
#
# This installs EVERYTHING, because the alternative cost a boot with an
# empty-looking /bin. `boot` used to depend on `programs`, which is the
# system's own programs and nothing else -- so every port was built,
# tested and absent, and `em`, `vi` and `which` were "missing" from a
# machine that had all three. Installing the lot is the default; a
# short loop on one program is `make programs`, which is still there.
boot: install
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
	$(MAKE) -C auth install

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
	$(MAKE) -C ports/libiconv install
	$(MAKE) -C ports/gettext install
	@echo
	@echo "Python is not in the list above -- it is 45 MB and 2,244 files,"
	@echo "and copying it takes minutes. 'make python' installs it, and"
	@echo "'make install' installs it along with everything else."

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
	$(MAKE) -C ports/libiconv install
	$(MAKE) -C ports/gettext install
	$(MAKE) -C ports/ncurses install

python: pylibs
	$(MAKE) -C ports/python install

# /etc/rc, replaced whatever it says. `make programs` installs it only
# when the disk has none or still has the unedited default.
etc:
	$(MAKE) -C system etc

# THE TOOLCHAIN THAT RUNS ON THE MACHINE (task 49).
#
# binutils and gcc, plus the C library installed at /usr where a
# compiler running there will look for it. Separate from `ports`
# because it is ~100 MB and a long build, and a machine that is only
# going to run programs does not need it.
toolchain:
	$(MAKE) -C libc install
	$(MAKE) -C ports/binutils install
	$(MAKE) -C ports/gmp install
	$(MAKE) -C ports/mpfr install
	$(MAKE) -C ports/mpc install
	$(MAKE) -C ports/libstdcxx install
	$(MAKE) -C ports/gcc install
	@echo
	@echo "the machine can now compile and link its own programs."

# THE KERNEL'S SOURCE, ON THE MACHINE.
#
# With the native toolchain already there, this is what makes the
# machine able to rebuild its own kernel and install it:
#
#   cd /usr/src/kernel && make install && reboot
#
# `copy-src` is the name to use; `src` is the same target under the
# name it had first, kept because `install` names it.
copy-src src:
	$(MAKE) -C kernel install-src

# The whole machine: the system, every port, Python, the toolchain, and
# the kernel's own source.
#
# `install` is the name to reach for -- it is what `boot` does and what
# somebody setting a disk up wants. `world` is kept because it is what
# this has always been called.
# A RECIPE, NOT A PREREQUISITE LIST, AND IN THIS ORDER.
#
# picolibc has to exist before anything that links against it -- ldso,
# every port and the native toolchain all do -- and `make install` on a
# machine that had never built it failed at the first port with
# "cross.sh: no picolibc ... run 'make libc'". A default that installs
# everything has to be able to do it from a clean tree, so it builds
# what it needs.
#
# Written as sequential $(MAKE) calls rather than prerequisites because
# prerequisites have no guaranteed order under `make -j`, and this one
# genuinely does: ports before libc is the same failure again.
install:
	$(MAKE) libc-if-missing
	$(MAKE) programs
	$(MAKE) ports
	$(MAKE) python
	$(MAKE) toolchain
	$(MAKE) src

world: install

# THE EMULATOR ITSELF.
#
# Not part of `install`, which fills the machine's disk -- this builds
# the machine. Separate because it is a host tool and a long build, and
# because nothing on the disk depends on it.
#
# It is here at all because the procedure was a block of shell in
# qemu-patch/README.md to be typed by hand, so the emulator existed
# only where somebody had last done that. A host whose QEMU predates a
# device gets a bus error inside that device's driver and a panic that
# reads like a kernel bug, which has happened once already.
qemu:
	qemu-patch/build.sh

# The kernel without the boot ROM in the way. Same kernel, quicker loop.
run:
	$(MAKE) -C kernel run

test: fsimgtest tests cryptotest fstest fattest apitest edittest vmtest nettest vttest libctest fscktest uemacstest vitest dnstest tcptest sotest pagetest devtest lotest awktest sedtest greptest sbasetest bashtest threadtest curstest logtest lesstest crontest ptytest pytest pylibtest dftest usertest logintest linktest sshtest whotest qemutest

# The host's end of the disk, before anything that uses it: every suite
# below stages its files through tools/fsimg.sh, so a fault in it does
# not look like a fault in it -- it looks like a program that was never
# installed.
fsimgtest:
	./tools/fsimgtest.sh

tests:
	$(MAKE) -C tests run

cryptotest:
	cd kernel && ./cryptotest.sh

fstest:
	cd kernel && ./fstest.sh

# FAT16 is no longer the machine's filesystem, only the fallback for a
# disk from somewhere else. Untested code in the kernel is worse than
# no code, so the fallback has a suite of its own.
fattest:
	cd kernel && ./fattest.sh

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

# Users, /etc/passwd and home directories, and what the mode bits now
# refuse.
logintest:
	cd kernel && ./logintest.sh

usertest:
	cd kernel && ./usertest.sh

# Hard links and symlinks: that a second name is the same inode and not
# a copy, that a link survives its first name being removed, and that a
# loop is ELOOP rather than a machine that has stopped.
linktest:
	cd kernel && ./linktest.sh

# ssh, scp and rsync against the workstation's own OpenSSH -- and TEN
# successive connections, because the machine once served exactly one
# and then transmitted nothing ever again.
sshtest:
	cd kernel && ./sshtest.sh

# who(1) and w(1): who the machine says is logged in, with somebody at
# the console AND somebody over ssh on a pty, and klogd running on the
# console to be left out. The first rule this program used reported the
# kernel's own idle task as a logged-in root and looked perfectly
# reasonable doing it.
whotest:
	cd kernel && ./whotest.sh

# The toolchain running on the machine: gcc and as and ld, compiling
# and linking programs that then run -- and the object files compared
# byte for byte against what the cross compiler makes of the same
# source. Wants more RAM and disk than the ordinary machine, and says
# so in the script.
#
# NOT in `make test`, deliberately: it needs `make toolchain` first,
# which is ~100 MB and a long build that a machine only meant to RUN
# programs does not need. A suite in the default list that cannot run
# without an optional build would either fail for everybody or pass
# vacuously, and this tree does not do vacuous passes.
nativetest:
	cd kernel && ./nativetest.sh

# A Sage040 program run by qemu-m68k's linux-user emulation, on this
# workstation, with none of this system underneath it. The ABI claim
# checked by somebody else's implementation of it.
qemutest:
	cd libc/test && ./qemutest.sh

# Every one of bash's own 83 tests, not the subset: hours, not minutes.
bashsuite:
	cd kernel && BASH_TESTS=all ./bashtest.sh

# picolibc, built and installed outside the tree (libc/README.md). Once.
#
# `make libc` always rebuilds, which is what somebody changing it wants.
# `libc-if-missing` is what `install` uses: building it again on every
# install would add minutes to a step that had nothing to do with it.
SAGE_LIBC ?= $(HOME)/m68k/sage040-libc

libc:
	libc/build.sh

# MISSING **OR STALE**.
#
# "Already built" is not the same as "built from this source". A second
# machine had a picolibc from before crypt.c was added to the overlay,
# and this said "already built" and went on -- so `make install` got
# all the way to linking /bin/login and stopped with "undefined
# reference to crypt", which names neither the C library nor the reason.
#
# Anything under libc/ newer than the installed library means rebuild.
# That is coarse: touching a README rebuilds picolibc, which takes a
# couple of minutes. Rebuilding when it was not needed costs minutes;
# NOT rebuilding when it was costs an error three steps away from its
# cause, which is the trade every stale-artifact bug in this tree has
# been on the wrong side of.
libc-if-missing:
	@if [ ! -f "$(SAGE_LIBC)/lib/libc.so" ]; then \
	    echo "picolibc: not built yet -- building it first"; \
	    $(MAKE) libc; \
	elif [ -n "$$(find libc -type f -newer $(SAGE_LIBC)/lib/libc.so \
	              -not -path 'libc/test/*' -print -quit 2>/dev/null)" ]; then \
	    echo "picolibc: $(SAGE_LIBC) is older than libc/ -- rebuilding"; \
	    $(MAKE) libc; \
	else \
	    echo "picolibc: up to date in $(SAGE_LIBC)"; \
	fi

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
	# Everything the test suites write lives in /tmp/scratch -- the disk
	# images most of all, which are 16 MB each and used to sit beside
	# the source with names that looked like part of it. hd.img is NOT
	# in there: that is the machine's own disk, not a build product.
	#
	rm -rf /tmp/scratch

distclean: clean disk-clean
