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

install: all $(DISK)
	@mmd -D s -i $(MIMG) ::/usr 2>/dev/null || true
	@# Every directory first, deepest last, then the files: mcopy -s
	@# will not create the tree for us on a FAT volume.
	@cd $(OUT) && find . -type d | sed 's|^\./||' | grep -v '^\.$$' | sort | \
	    while read d; do mmd -D s -i $(MIMG) "::/usr/$$d" 2>/dev/null || true; done
	@cd $(OUT) && find . -type f | sed 's|^\./||' | \
	    while read f; do mcopy -o -i $(MIMG) "$$f" "::/usr/$$f"; done
	@echo "gcc is on $(DISK) at /usr"

clean:
	rm -rf $(OUT)
