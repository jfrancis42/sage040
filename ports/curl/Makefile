# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# curl: fetched and built by build.sh into ~/m68k/src. Nothing built
# lands in this directory.
#
# https needs the trust store, so installing curl installs ports/ca-certs
# too: a curl that cannot verify any server is not a working curl.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-curl-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(MAKE) -s -C ../ca-certs install DISK=$(abspath $(DISK))
	@$(FSIMG) mkdir /bin
	$(FSIMG) put -m 755 $(OUT)/bin/curl /bin/curl
	@echo "curl is on $(DISK)"

clean:
	rm -rf $(OUT)
