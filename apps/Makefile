# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Applications: things somebody chose to run on the machine.
#
# A spinning cube, a web fetcher, a web server, and the programs that
# exist to be tested against -- hello, spin, faulter. None of them is
# part of the system and the machine works perfectly with the lot
# deleted, which is the distinction this directory draws.
#
# They install in the root rather than /bin, and PATH finds them there
# because it ends with "." -- last, so that something dropped here
# cannot quietly replace a system program of the same name.

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := cube fbtest hello fetch httpd spin faulter napper cdtest statfs memtest malloctest sigtest fptest polltest timetest pipetest
INSTALL_DIR :=

include $(TOPDIR)/lib/program.mk
