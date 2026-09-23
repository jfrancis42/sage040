# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Dropbear: ssh, sshd, scp. Fetched and built by build.sh into
# ~/m68k/src; nothing built lands in this directory.
#
# These go in /BIN with the rest of the machine's programs. The client
# is installed TWICE, as `dbclient` and as `ssh`: rsync runs "ssh" by
# name, as does everybody's muscle memory, and FAT has no symbolic
# links to point one at the other.
#
# The server needs a host key before it will run, and making one is a
# thing the MACHINE does once, not something shipped in the image -- a
# host key that arrived with the operating system is a host key
# everybody else has too. `dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key`
# is the whole of it; /etc/rc could do it on first boot.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

SRCDIR ?= $(HOME)/m68k/src
OUT    := $(SRCDIR)/build-dropbear-sage040/sage040

.DEFAULT_GOAL := all
.PHONY: all install clean

all:
	./build.sh

install: all $(DISK)
	@$(FSIMG) mkdir /bin
	@$(FSIMG) mkdir /etc
	@for b in dropbear dbclient ssh scp dropbearkey dropbearconvert; do \
	    if [ -x $(OUT)/bin/$$b ]; then \
	        $(FSIMG) put -m 755 $(OUT)/bin/$$b /bin/$$b; \
	    fi; \
	done
	@echo "dropbear is on $(DISK): ssh, dropbear, scp, dropbearkey"

clean:
	rm -rf $(OUT)
