# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# git: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# What goes on the machine: /usr/bin/git and its few companions,
# /usr/libexec/git-core (the helpers, the shell and Perl commands, and
# git-remote-http for https), and /usr/share/perl5 (Git.pm and friends,
# which the Perl commands load from there by name) and git-core's
# templates.
#
# fsimg's `put -r` copies regular files only, so the symbolic links
# git installs (git-remote-https -> git-remote-http, git-upload-pack
# -> git, ...) are made afterwards, from the build's own list of them.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-git-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean test

all:
	./build.sh

install: all $(DISK)
	@for d in /usr /usr/bin /usr/libexec /usr/libexec/git-core /usr/share \
	          /usr/share/perl5 /usr/share/git-core; do $(FSIMG) mkdir $$d; done
	$(FSIMG) put -r $(OUT)/bin /usr/bin
	$(FSIMG) put -r $(OUT)/libexec/git-core /usr/libexec/git-core
	$(FSIMG) put -r $(OUT)/share/perl5 /usr/share/perl5
	$(FSIMG) put -r $(OUT)/share/git-core /usr/share/git-core
	@cd $(OUT) && find bin libexec -type l | while read -r l; do \
	    $(ABS_FSIMG) symlink "$$(readlink "$$l")" "/usr/$$l" || exit 1; \
	 done
	@echo "git is on $(DISK)"

test:
	cd $(TOPDIR)/kernel && ./gittest.sh

clean:
	rm -rf $(OUT)
