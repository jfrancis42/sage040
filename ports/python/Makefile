# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# CPython: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# What goes on the machine: /usr/local/bin/python3 and the standard
# library under /usr/local/lib/python3.14, which is where the
# interpreter looks because that is the prefix it was built with.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR  ?= $(HOME)/m68k/src
PYBUILD := $(SRCDIR)/build-python-sage040
PYOUT   := $(PYBUILD)/sage040/usr/local
PYVER   := 3.14
STRIP   ?= $(HOME)/m68k/install/bin/m68k-elf-strip
BUILD_PYTHON ?= python3

# The standard library, minus what a machine this size should not carry:
# the test suites (25 MB of them), the .py sources whose .pyc are
# beside them, and the packaging tools that need a network index.
PRUNE := test tests idlelib turtledemo tkinter ensurepip pydoc_data \
         lib2to3 __pycache__/*.opt-?.pyc

.DEFAULT_GOAL := all
.PHONY: all install install-bin install-lib stage clean test

all:
	./build.sh

# A staged copy, pruned, so that what goes on the disk is deliberate
# and the same every time.
stage: all
	rm -rf $(PYBUILD)/stage
	mkdir -p $(PYBUILD)/stage/bin $(PYBUILD)/stage/lib
	cp $(PYOUT)/bin/python$(PYVER) $(PYBUILD)/stage/bin/python3
	$(STRIP) $(PYBUILD)/stage/bin/python3
	cp -r $(PYOUT)/lib/python$(PYVER) $(PYBUILD)/stage/lib/
	@for p in $(PRUNE); do \
	    find $(PYBUILD)/stage/lib/python$(PYVER) -depth -name "$$p" \
	        -exec rm -rf {} + 2>/dev/null || true; \
	done
	@# HASH-BASED .pyc, and unchecked at that.
	@#
	@# A .pyc normally carries the source's SIZE AND MODIFICATION TIME
	@# and is used only if both still match. The times here are
	@# whatever mcopy wrote onto a FAT filesystem, which are not the
	@# times the host build saw -- so every module looked stale, and
	@# the machine recompiled the whole standard library from source,
	@# on a 25 MHz 68040, at every import. `statistics` alone took
	@# minutes.
	@#
	@# --invalidation-mode unchecked-hash writes a .pyc that says "use
	@# me", and the interpreter does not stat the source at all. The
	@# cost is that editing a .py on the machine has no effect until
	@# its .pyc is removed; the alternative is a machine that spends
	@# its life compiling Python it was given already compiled.
	@$(BUILD_PYTHON) -m compileall -q -f --invalidation-mode unchecked-hash \
	    $(PYBUILD)/stage/lib/python$(PYVER) > /dev/null 2>&1 || true
	@echo "staged: $$(du -sh $(PYBUILD)/stage | cut -f1), \
$$(find $(PYBUILD)/stage -type f | wc -l) files"

install-bin: stage $(DISK)
	@mmd -D s -i $(MIMG) ::/usr 2>/dev/null || true
	@mmd -D s -i $(MIMG) ::/usr/local 2>/dev/null || true
	@mmd -D s -i $(MIMG) ::/usr/local/bin 2>/dev/null || true
	mcopy -o -i $(MIMG) $(PYBUILD)/stage/bin/python3 ::/usr/local/bin/python3
	@mmd -D s -i $(MIMG) ::/BIN 2>/dev/null || true
	mcopy -o -i $(MIMG) $(PYBUILD)/stage/bin/python3 ::/BIN/python3

# mcopy -s copies a directory tree, which is what the standard library
# is: about 1,700 files once the test suites are gone.
install-lib: stage $(DISK)
	@mmd -D s -i $(MIMG) ::/usr/local/lib 2>/dev/null || true
	mcopy -o -s -i $(MIMG) $(PYBUILD)/stage/lib/python$(PYVER) ::/usr/local/lib/
	@echo "the standard library is on $(DISK)"

install: install-bin install-lib

test:
	cd $(TOPDIR)/kernel && ./pytest.sh

clean:
	rm -rf $(PYBUILD)/stage $(PYBUILD)/sage040
