# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# ncurses: fetched and built by build.sh into ~/m68k/src (nothing built
# lands in this directory -- see the note at the top of build.sh).
#
# What goes on the machine: the terminfo database at /usr/share/terminfo
# and the programs that read it. The libraries stay on the host, because
# a static library is something to link against, not something to run.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR  ?= $(HOME)/m68k/src
NCOUT   := $(SRCDIR)/build-ncurses-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install install-db install-progs install-test curstest clean test

CC_SAGE   ?= $(M68K_PREFIX)/bin/m68k-elf-gcc
M68K_PREFIX ?= $(HOME)/m68k/install
SAGE_LIBC ?= $(HOME)/m68k/sage040-libc

all:
	./build.sh

# The database, directory by directory. mmd -D s leaves an existing one
# alone; mcopy -o replaces the files.
install-db: all $(DISK)
	@mmd -D s -i $(MIMG) ::/usr 2>/dev/null || true
	@mmd -D s -i $(MIMG) ::/usr/share 2>/dev/null || true
	@mmd -D s -i $(MIMG) ::/usr/share/terminfo 2>/dev/null || true
	@for d in $(NCOUT)/terminfo/*/; do \
	    n=$$(basename $$d); \
	    mmd -D s -i $(MIMG) ::/usr/share/terminfo/$$n 2>/dev/null || true; \
	    mcopy -o -i $(MIMG) $$d* ::/usr/share/terminfo/$$n/ ; \
	done
	@echo "terminfo: $$(find $(NCOUT)/terminfo -type f | wc -l) terminals on $(DISK)"

install-progs: all $(DISK)
	@mmd -D s -i $(MIMG) ::/BIN 2>/dev/null || true
	@for p in tput tset infocmp clear tabs; do \
	    [ -x $(NCOUT)/bin/$$p ] && \
	    mcopy -o -i $(MIMG) $(NCOUT)/bin/$$p ::/BIN/$$p ; \
	done
	@echo "ncurses programs: tput tset infocmp clear tabs"

install: install-db install-progs install-test

# The test program, linked against the libraries just built. It is a
# program like any other: dynamic, against /lib/libc.so.
CURSTEST := $(NCOUT)/bin/curstest

$(CURSTEST): test/curstest.c all
	$(CC_SAGE) -mcpu=68040 -O2 -Wall -Wextra -nostdinc -nostdlib \
	    -isystem $(NCOUT)/include \
	    -isystem $(SAGE_LIBC)/include \
	    -isystem $$($(CC_SAGE) -print-file-name=include) \
	    -D_GNU_SOURCE \
	    -Wl,-Ttext-segment=0x10000000 -Wl,--dynamic-linker=/lib/ld.so \
	    -Wl,-z,now -Wl,--hash-style=sysv -Wl,--build-id=none \
	    $(TOPDIR)/libc/crt0-dyn.s test/curstest.c \
	    -L$(NCOUT)/lib -lncursesw -ltinfow \
	    -L$(SAGE_LIBC)/lib -lc -lgcc -o $@

curstest: $(CURSTEST)

install-test: $(CURSTEST) $(DISK)
	@mmd -D s -i $(MIMG) ::/BIN 2>/dev/null || true
	mcopy -o -i $(MIMG) $(CURSTEST) ::/BIN/curstest

test:
	cd $(TOPDIR)/kernel && ./curstest.sh

clean:
	rm -rf $(SRCDIR)/build-ncurses-sage040/sage040
