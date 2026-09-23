# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# gcc, native: fetched and built by build.sh into ~/m68k/src. Nothing
# built lands in this directory.
#
# It installs into /usr on the machine -- /usr/bin for the driver,
# /usr/libexec for cc1 and cc1plus, /usr/lib/gcc for the compiler's own
# headers. That is the prefix it was configured with, so the driver
# finds its own pieces without being told where they are.
#
# `make -C libc install` has to have been run: a compiler running on
# the machine needs the C library's headers at /usr/include and its
# libraries, start files and linker script at /usr/lib. `make
# toolchain` at the top level does both, in order.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-gcc-native/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

# AN ABSOLUTE PATH TO THE IMAGE, because the loop below cd's into the
# build output and $(FSIMG) is relative to the project root -- after
# the cd it named nothing. $(ABS_FSIMG) is the same thing spelled
# absolutely, which is what a recipe that has moved has to use.
# while the disk sat there perfectly writable.

# share/ is left behind on purpose: it is manual pages and info files
# and locale catalogues, and this machine has no man, no info and one
# language. The compiler does not read any of it.
install: all $(DISK)
	@$(FSIMG) mkdir /usr
	@# Directories first, shallowest first, because a file cannot be
	@# written into a directory that is not there yet.
	@cd $(OUT) && find . -type d -not -path './share*' | sed 's|^\./||' | \
	    grep -v '^\.$$' | sort | \
	    while read d; do $(ABS_FSIMG) mkdir /usr/$$d; done
	@cd $(OUT) && find . -type f -not -path './share/*' | sed 's|^\./||' | \
	    while read f; do $(ABS_FSIMG) put -m 755 $$f /usr/$$f || exit 1; done
	@echo "gcc is on $(DISK) at /usr"

clean:
	rm -rf $(OUT)
