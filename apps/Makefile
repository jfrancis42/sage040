# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Applications: things somebody chose to run on the machine.
#
# A spinning cube, a flock of boids, Wa-Tor, Life, strange attractors,
# plasma, fire, the Mandelbrot set, a voxel landscape, more automata, a
# sandpile, DLA, stars, balls, sorts and reaction-diffusion, a
# web fetcher, a web server, and the programs that exist to be tested against -- hello,
# spin, faulter. None of them is part of the system and the machine works
# perfectly with the lot deleted, which is the distinction this directory
# draws.
#
# All of them are BUILT, because the test suites stage the fixtures they
# need straight from the binaries here. But only the ones a person would
# actually run -- the graphics demos, the web fetcher and the web
# server -- are INSTALLED, and to /bin like any other
# command. The rest exist to be tested against, and putting them in the
# machine's root just filled it with clutter that shadowed nothing and
# served no one.

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := cube boids wator life attractor plasma fire mandel voxel automata sandpile dla stars balls sorts rd fpsptest faulttest fbtest hello fetch httpd spin faulter napper cdtest statfs memtest malloctest sigtest fptest polltest timetest pipetest proctest procfstest socktest vtcheck winsize vcsnap tcptest pagetest limits fsstress fbmap udpwait lineprobe
INSTALL_PROGS := cube boids wator life attractor plasma fire mandel voxel automata sandpile dla stars balls sorts rd fetch httpd
INSTALL_DIR := /bin

include $(TOPDIR)/lib/program.mk
