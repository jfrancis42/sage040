# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# lynx: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# Installs /bin/lynx, and /etc/lynx.cfg and /etc/lynx.lss ONLY IF THEY
# ARE NOT THERE: they are the owner's to edit, as /etc/passwd is. https
# needs the trust store, so ports/ca-certs is installed too -- and so
# are gzip and brotli: Lynx decodes a compressed page only if it finds
# the matching PROGRAM on PATH, even with the library linked in. The
# terminal descriptions come from ports/ncurses (`make ports` installs
# it); without them Lynx still has vt100 and vt102 compiled in.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-lynx-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(MAKE) -s -C ../ca-certs install DISK=$(abspath $(DISK))
	@$(MAKE) -s -C ../gzip install DISK=$(abspath $(DISK))
	@$(MAKE) -s -C ../brotli install DISK=$(abspath $(DISK))
	@$(FSIMG) mkdir /bin
	@$(FSIMG) mkdir /etc
	$(FSIMG) put -m 755 $(OUT)/bin/lynx /bin/lynx
	@for f in lynx.cfg lynx.lss; do \
	    if ! $(FSIMG) exists /etc/$$f; then \
	        $(FSIMG) put -m 644 $(OUT)/etc/$$f /etc/$$f && echo "installed /etc/$$f"; \
	    else \
	        echo "/etc/$$f is already there, and is yours: not touched"; \
	    fi; \
	done
	@echo "lynx is on $(DISK)"

clean:
	rm -rf $(OUT)
