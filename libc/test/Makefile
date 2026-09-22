# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Programs that test the C library. They install into the root, like
# apps/, and are built against picolibc (libc.mk), not lib/ulib.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := libctest

include $(TOPDIR)/libc/libc.mk
