# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# ca-certs: Mozilla's root store, fetched and checked by build.sh into
# ~/m68k/src/ca-certs, installed as /etc/ssl/cert.pem -- the path
# OpenSSL's default trust store names, because ports/openssl is built
# with --openssldir=/etc/ssl.
#
# Unlike /etc/rc and /etc/passwd this is not the owner's file to edit:
# it is replaced on every install, the way a distribution's
# ca-certificates package replaces it. A CA of one's own belongs in a
# file of its own, named with SSL_CERT_FILE or -CAfile.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/ca-certs

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /etc/ssl
	@$(FSIMG) put -m 644 $(OUT)/cert.pem /etc/ssl/cert.pem
	@echo "/etc/ssl/cert.pem is on $(DISK)"

clean:
	rm -rf $(OUT)
