# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# The system's own programs, which live in /bin.
#
# These are part of the machine rather than things somebody chose to run
# on it: configuring the interface, asking whether a host is reachable,
# showing what the network layer is holding, stopping the machine. A
# fresh disk is expected to have them and PATH looks here first.
#
# They are ordinary unprivileged programs all the same. ifconfig runs in
# an address space of its own and can do exactly what the system call
# interface allows -- being part of the system buys it no privilege,
# which is the property worth having.

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := ifconfig ping netstat shutdown env stty resize sh
INSTALL_DIR := /BIN

include $(TOPDIR)/lib/program.mk

# sh is the kernel's own shell, built as a program: the shell and line
# editor sources, the gate's calling side, and the pure helpers they use
# -- and NOT ulib, whose string functions would collide with the
# kernel's own string.c that the shell is written against.
K := $(TOPDIR)/kernel
SH_SRCS := sh.c $(K)/shell.c $(K)/edit.c $(K)/sysuser.c $(K)/string.c \
           $(K)/errno.c $(K)/time.c

sh: $(SH_SRCS) $(LIB)/crt0.s $(LIB)/user.ld $(K)/syscall.h $(K)/uapi.h \
    $(TOPDIR)/types.h $(K)/edit.h
	$(CC) $(CFLAGS) $(LDFLAGS) \
	    -x assembler-with-cpp $(LIB)/crt0.s \
	    -x c $(SH_SRCS) -o $@
	@$(SIZE) $@
