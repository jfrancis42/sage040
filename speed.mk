# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# speed.mk - how fast the emulated machine runs.
#
# QEMU's -icount charges every guest instruction 2^SPEED nanoseconds, so
# the machine executes 10^9 / 2^SPEED instructions per second and
# virtual time becomes deterministic: the same run takes the same number
# of guest seconds whatever else the host is doing.
#
#   SPEED=5    31.2 M instr/sec   faster than any 68040 ever shipped
#   SPEED=6    15.6 M instr/sec   about a 25 MHz 68040   <- default
#   SPEED=7     7.8 M instr/sec   about a 68020
#   SPEED=off  host speed, non-deterministic
#
# THE DEFAULT IS THE REAL MACHINE. Software that feels right on a
# 25 MHz 68040 is the whole point of this; one that runs at whatever
# the host manages says nothing about that, and makes a program that
# would crawl on the hardware feel fine.
#
# Override it wherever waiting is the only cost -- compiling ON the
# machine most of all, which is 40 minutes for the kernel at 25 MHz:
#
#   make -C kernel run SPEED=off      as fast as this host will go
#   make -C kernel boot SPEED=5       quicker, still deterministic
#
# THE TEST SUITES DO NOT COME THROUGH HERE and are deliberately
# unthrottled: they check behaviour rather than timing, several already
# take minutes, and `-icount` makes a slow suite slower without making
# it a better test.
#
# Powers of two only; programmer-guide.md says what this does and does
# not model.

SPEED ?= 6

ifeq ($(SPEED),off)
  ICOUNT :=
else
  ICOUNT := -icount shift=$(SPEED),sleep=on
endif
