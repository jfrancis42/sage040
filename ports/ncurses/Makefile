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

# The database, directory by directory. mkdir leaves an existing one
# alone; put replaces the files.
install-db: all $(DISK)
	@$(FSIMG) mkdir /usr
	@$(FSIMG) mkdir /usr/share
	@$(FSIMG) mkdir /usr/share/terminfo
	@for d in $(NCOUT)/terminfo/*/; do \
	    n=$$(basename $$d); \
	    $(FSIMG) mkdir /usr/share/terminfo/$$n; \
	    $(FSIMG) put $$d* /usr/share/terminfo/$$n/ ; \
	done
	@echo "terminfo: $$(find $(NCOUT)/terminfo -type f | wc -l) terminals on $(DISK)"

install-progs: all $(DISK)
	@$(FSIMG) mkdir /bin
	@for p in tput tset infocmp clear tabs; do \
	    [ -x $(NCOUT)/bin/$$p ] && \
	    $(FSIMG) put -m 755 $(NCOUT)/bin/$$p /bin/$$p ; \
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
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(CURSTEST) /bin/curstest

test:
	cd $(TOPDIR)/kernel && ./curstest.sh

clean:
	rm -rf $(SRCDIR)/build-ncurses-sage040/sage040
