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
# Beside OUT, not in it: install copies everything under OUT to /usr.
SPECS  := $(SRCDIR)/build-gcc-native/specs.sage040

# A driver configured as the native one, run on this host for its
# -dumpspecs: the cross compiler build.sh builds the native one with
# (SAGE_CXX, as there). Its dump is the native driver's, line for line.
SAGE_CXX  ?= $(HOME)/m68k/install-cxx
SPECS_GCC := $(SAGE_CXX)/bin/m68k-elf-gcc

.DEFAULT_GOAL := all
.PHONY: all install clean specs

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
install: all $(DISK) $(SPECS)
	@$(FSIMG) mkdir /usr
	@# Directories first, shallowest first, because a file cannot be
	@# written into a directory that is not there yet.
	@cd $(OUT) && find . -type d -not -path './share*' | sed 's|^\./||' | \
	    grep -v '^\.$$' | sort | \
	    while read d; do $(ABS_FSIMG) mkdir /usr/$$d; done
	@cd $(OUT) && find . -type f -not -path './share/*' | sed 's|^\./||' | \
	    while read f; do $(ABS_FSIMG) put -m 755 $$f /usr/$$f || exit 1; done
	@# Make `gcc hello.c -o hello` work with no -specs= flag. gcc reads a
	@# file named `specs` in its own version directory at startup --
	@# INSTEAD of its built-in specs, so the file has to be all of them
	@# (mkspecs.py says what went wrong when it was only ours). It is the
	@# full -dumpspecs of a driver configured as this one, with the four
	@# specs libc/sage040.specs changes put in: this system's start
	@# files, libraries and link, not a hosted GCC's crtbegin.o.
	@vdir=$$(cd $(OUT) && find lib/gcc -mindepth 2 -maxdepth 2 -type d | head -1); \
	    if [ -z "$$vdir" ]; then \
	        echo "gcc install: no lib/gcc/<target>/<version> directory" >&2; exit 1; \
	    fi; \
	    $(ABS_FSIMG) put $(SPECS) /usr/$$vdir/specs
	@echo "gcc is on $(DISK) at /usr; plain 'gcc hello.c -o hello' works, no -specs= flag"

# The specs file gcc reads at startup: all of gcc's own, with this
# system's start files, libraries and link put in (mkspecs.py). Made
# here once; install and kernel/nativetest.sh both put this file on
# their disk, so the one a test checks is the one the machine gets.
specs: $(SPECS)

$(SPECS): mkspecs.py $(TOPDIR)/libc/sage040.specs
	@mkdir -p $(dir $(SPECS))
	$(SPECS_GCC) -dumpspecs > $@.full
	python3 mkspecs.py $@.full $(abspath $(TOPDIR)/libc/sage040.specs) > $@.tmp
	@grep -q '^\*asm_cpu_spec:' $@.tmp || \
	    { echo "gcc specs: no asm_cpu_spec in the result" >&2; exit 1; }
	@mv $@.tmp $@

clean:
	rm -rf $(OUT)
