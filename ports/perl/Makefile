# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Perl 5: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# What goes on the machine: /usr/bin/perl, the programs that come with
# it (perldoc, prove, cpan, h2xs, xsubpp ...), and the library under
# /usr/lib/perl5/5.44.0 -- which is where the interpreter looks, because
# that is the prefix it was built with. The scripts start
# #!/usr/bin/perl, which the kernel follows.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR  ?= $(HOME)/m68k/src
OUT     := $(SRCDIR)/build-perl-sage040/sage040
VERSION := 5.44.0

.DEFAULT_GOAL := all
.PHONY: all install install-bin install-lib clean test

all:
	./build.sh

install-bin: all $(DISK)
	@$(FSIMG) mkdir /usr
	@$(FSIMG) mkdir /usr/bin
	@for f in $(OUT)/bin/*; do \
	    $(FSIMG) put -m 755 $$f /usr/bin/$$(basename $$f) || exit 1; \
	 done
	@echo "perl and $$(ls $(OUT)/bin | wc -l) programs are in /usr/bin"

# `fsimg put -r SRC DST` copies SRC's CONTENTS into DST, so the
# destination names the directory itself (see ports/python/Makefile).
install-lib: all $(DISK)
	@$(FSIMG) mkdir /usr/lib
	@$(FSIMG) mkdir /usr/lib/perl5
	$(FSIMG) put -r $(OUT)/lib/perl5/$(VERSION) /usr/lib/perl5/$(VERSION)
	@echo "the Perl library is on $(DISK)"

install: install-bin install-lib

test:
	cd $(TOPDIR)/kernel && ./perltest.sh

clean:
	rm -rf $(OUT)
