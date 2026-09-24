# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
# Makefile - Sage040 rotating wireframe cube
#
#   make        build cube.elf
#   make run    build and run it in a window
#   make shot   build, run headless, and capture docs/cube.png

# Toolchain and emulator locations.
#
# Both default to the local build described in the README, and fall back to
# whatever is on PATH if that is not present. Override either on the command
# line, e.g.  make M68K_PREFIX=/opt/m68k  or  make QEMU=/path/to/qemu-system-m68k
M68K_PREFIX ?= $(HOME)/m68k/install
SAGE_QEMU   ?= $(HOME)/m68k/sage040-qemu

TOOLCHAIN := $(if $(wildcard $(M68K_PREFIX)/bin/m68k-elf-gcc),$(M68K_PREFIX)/bin/m68k-elf,m68k-elf)
QEMU      ?= $(if $(wildcard $(SAGE_QEMU)/bin/qemu-system-m68k),$(SAGE_QEMU)/bin/qemu-system-m68k,qemu-system-m68k)
CC        := $(TOOLCHAIN)-gcc
OBJDUMP   := $(TOOLCHAIN)-objdump
SIZE      := $(TOOLCHAIN)-size

TOPDIR := ..
include $(TOPDIR)/machine.conf

MACHINE   := -M sage040 -cpu m68040 -m $(RAM_MB)

# SPEED / ICOUNT, shared with kernel and bootrom. This defaulted to 6
# here long before the rest of the tree did; speed.mk is that default,
# moved somewhere all three can read it.
include $(TOPDIR)/speed.mk

SUP       := ../tests
CPUFLAGS  := -mcpu=68040
CFLAGS    := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
             -Wall -Wextra -fno-builtin -fno-stack-protector -I.. -I$(SUP)
LDFLAGS   := $(CPUFLAGS) -ffreestanding -nostdlib -T $(SUP)/sage040.ld \
             -Wl,--build-id=none -Wl,--no-warn-rwx-segments

DEPS := $(SUP)/crt0.s $(SUP)/uart.c $(SUP)/mfp.c $(SUP)/sage040.h $(SUP)/sage040.ld

.PHONY: all run run-float shot disasm clean compare

all: cube.elf cube-float.elf

cube.elf: cube.c $(DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) \
	    -x assembler-with-cpp $(SUP)/crt0.s \
	    -x c $(SUP)/uart.c $(SUP)/mfp.c cube.c -o $@
	@$(SIZE) $@

cube-float.elf: cube.c $(DEPS)
	$(CC) $(CFLAGS) -DUSE_FLOAT=1 $(LDFLAGS) \
	    -x assembler-with-cpp $(SUP)/crt0.s \
	    -x c $(SUP)/uart.c $(SUP)/mfp.c cube.c -o $@
	@$(SIZE) $@

compare: cube.elf cube-float.elf
	./compare.sh

run: cube.elf
	$(QEMU) $(MACHINE) $(ICOUNT) -kernel cube.elf -serial stdio

run-float: cube-float.elf
	$(QEMU) $(MACHINE) $(ICOUNT) -kernel cube-float.elf -serial stdio

shot: cube.elf
	./shot.sh

disasm: cube.elf
	$(OBJDUMP) -d cube.elf

clean:
	rm -f cube.elf cube-float.elf *.ppm *.out
