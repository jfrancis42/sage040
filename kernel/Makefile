# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Sage040 kernel
#
# make build kernel.elf and kernel.rom
# make run run the kernel directly, with the disk attached
# make install write kernel.rom onto the disk as KERNEL.ROM
# make boot install it, then boot the machine through the boot ROM
# make clean remove build artifacts (the disk image is not one)

# Toolchain and emulator locations.
#
# Both default to the local build described in the README, and fall back to
# whatever is on PATH if that is not present. Override either on the command
# line, e.g. make M68K_PREFIX=/opt/m68k or make QEMU=/path/to/qemu-system-m68k
M68K_PREFIX ?= $(HOME)/m68k/install
SAGE_QEMU ?= $(HOME)/m68k/sage040-qemu

TOOLCHAIN := $(if $(wildcard $(M68K_PREFIX)/bin/m68k-elf-gcc),$(M68K_PREFIX)/bin/m68k-elf,m68k-elf)
QEMU ?= $(if $(wildcard $(SAGE_QEMU)/bin/qemu-system-m68k),$(SAGE_QEMU)/bin/qemu-system-m68k,qemu-system-m68k)

CC := $(TOOLCHAIN)-gcc
OBJCOPY := $(TOOLCHAIN)-objcopy
OBJDUMP := $(TOOLCHAIN)-objdump
SIZE := $(TOOLCHAIN)-size
NM := $(TOOLCHAIN)-nm

TOPDIR := ..
include $(TOPDIR)/disk.mk

# disk.mk defines a target before this file does, so say which is the
# default rather than letting include order decide it.
.DEFAULT_GOAL := all

MACHINE := -M sage040 -cpu m68040 -m $(RAM_MB)

# How fast the machine runs: SPEED, default 6 (~25 MHz 68040).
# Overridable -- see speed.mk, which kernel, bootrom and cube share.
include $(TOPDIR)/speed.mk

# Hardware register definitions are shared with the device tests.
SUP := ../tests
CPUFLAGS := -mcpu=68040
CFLAGS := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
 -Wall -Wextra -Werror -fno-builtin -fno-stack-protector \
 -DSAGE040_NO_TESTLIB \
 -I. -I.. -Idrivers -Inet -I$(SUP)
LDFLAGS := $(CPUFLAGS) -ffreestanding -nostdlib -T kernel.ld \
 -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map=kernel.map

# THE SOURCE LIST LIVES IN `SOURCES`, not here.
#
# Read by this Makefile and by build.sh, which is the same build run
# ON the machine with its own compiler. Two copies of this list is a
# kernel that builds on the workstation and fails to link natively,
# discovered by whoever tries it next -- so there is one copy, and
# both builds split it on the extension.
ALLSRC := $(shell sed -e 's/\#.*//' SOURCES)
SRCS   := $(filter %.c,$(ALLSRC))

# Every header, by wildcard. The list used to be written out and had
# fallen well behind -- task.h, signal.h and most of the rest were not
# on it, so a change to one alone rebuilt nothing.
HDRS := $(wildcard *.h drivers/*.h fs/*.h net/*.h) ../types.h \
 $(SUP)/sage040.h

.PHONY: all run install install-src boot disasm syms clean distclean

all: kernel.rom

ASMS := $(filter %.s,$(ALLSRC))

# The layering rule is checked before the link, not documented and hoped
# for. shell.c and edit.c are programs: they reach the system through
# trap #0 and may not include a kernel header that lets them go round it.
# See layercheck.sh for what is allowed and why.
#
# So is the ABI: every system call number Linux/m68k has must be Linux's
# number (abicheck.sh, against linux-m68k-syscalls.txt).
kernel.elf: $(ASMS) $(SRCS) $(HDRS) kernel.ld layercheck.sh abicheck.sh \
            linux-m68k-syscalls.txt SOURCES
	@./layercheck.sh
	@./abicheck.sh
	$(CC) $(CFLAGS) $(LDFLAGS) \
	 -x assembler-with-cpp $(ASMS) \
	 -x c $(SRCS) -o $@
	@$(SIZE) $@

# The flat image the boot ROM loads: the vector table first, so its first
# two longwords are the initial SSP and PC, then everything else.
kernel.rom: kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "kernel.rom: $$(stat -c%s $@) bytes"

# Run the kernel straight from the ELF. Quicker to iterate on than a full
# boot, and the disk is attached either way, so the filesystem works.
#
# -no-reboot is what makes `shutdown` work. The machine stops by telling
# the keyboard controller to pull the reset line, the way a PC always
# has; without this the emulator would dutifully reset and start over,
# and the one command whose whole job is getting out would be the one
# that could not.
#
# ../tools/qemu-net.sh works out how THIS host is connected and puts the
# guest on the real network if it can -- a bridge if one exists, a
# macvtap on a wired interface otherwise, and QEMU's user-mode NAT if
# neither is possible (a laptop on Wi-Fi, for one: 802.11 cannot carry a
# second machine's MAC). Override with SAGE_NET=slirp, lan or none.
#
# The test suites deliberately do not use it: they run on slirp so they
# need no privileges and touch nothing outside the emulator.
#
NET := ../tools/qemu-net.sh

# The serial console is a plain stdio chardev with signal=off, NOT
# `-serial mon:stdio`. `mon:` multiplexes the QEMU monitor onto the same
# stream and reserves Ctrl-A as its escape prefix -- so Ctrl-A never
# reached the guest, and bash's "go to the start of the line" did
# nothing. signal=off hands Ctrl-C to the guest too, so it interrupts a
# program instead of killing the emulator. The monitor, when wanted, goes
# on its own socket (`-monitor unix:...`), which is how the screenshot
# tooling already reaches it. To stop a wedged run, kill QEMU from
# another terminal: pkill -f 'qemu-system-m6[8]k'.
run: kernel.elf $(DISK)
	$(NET) $(QEMU) $(MACHINE) $(ICOUNT) -no-reboot -kernel kernel.elf \
	 -drive file=$(DISK),format=raw,if=ide \
	 -chardev stdio,id=con,signal=off -serial chardev:con

# Put the kernel on the disk where the boot ROM will find it.
install: kernel.rom $(DISK)
	@$(FSIMG) put kernel.rom /KERNEL.ROM
	@echo "KERNEL.ROM: $$(stat -c%s kernel.rom) bytes -> $(DISK) partition 1"

# The real thing: the boot ROM finds KERNEL.ROM on the filesystem and runs it.
boot: install
	$(MAKE) -C ../bootrom boot SPEED=$(SPEED)

# THE KERNEL'S OWN SOURCE, ON THE MACHINE, SO IT CAN REBUILD ITSELF.
#
# Staged into a tree first and copied in one go: `fsimg put -r` is a
# single debugfs session for the whole thing, where a file at a time is
# 120 process starts.
#
# The layout keeps the include paths the build already uses -- the
# kernel compiles with -I.. and -I../tests, so types.h and sage040.h
# have to sit beside the kernel directory exactly as they do here. That
# is why this copies to /usr/src and not to /usr/src/kernel alone.
#
#   /usr/src/types.h
#   /usr/src/tests/sage040.h
#   /usr/src/kernel/...
#
# Build it there with ./build.sh, not make: see the head of that script
# for why.
install-src: $(DISK)
	@rm -rf .srcstage
	@mkdir -p .srcstage/kernel/drivers .srcstage/kernel/net \
	          .srcstage/kernel/fs .srcstage/tests
	@cp ../types.h .srcstage/
	@cp $(SUP)/sage040.h .srcstage/tests/
	@cp *.c *.h *.s kernel.ld SOURCES build.sh \
	    layercheck.sh abicheck.sh linux-m68k-syscalls.txt \
	    .srcstage/kernel/
	@cp Makefile.native .srcstage/kernel/Makefile
	@# Whatever each one HAS: fs/ has no headers, and a bare
	@# `cp fs/*.h` fails the whole target on the glob that matched
	@# nothing rather than on anything being wrong.
	@for d in drivers net fs; do \
	    for f in $$d/*.c $$d/*.h; do \
	        [ -f "$$f" ] && cp "$$f" .srcstage/kernel/$$d/; \
	    done; \
	done; true
	@chmod 755 .srcstage/kernel/build.sh .srcstage/kernel/layercheck.sh \
	           .srcstage/kernel/abicheck.sh
	@$(FSIMG) put -r .srcstage /usr/src
	@rm -rf .srcstage
	@echo "kernel source -> /usr/src  ($$(find . -path ./.srcstage -prune -o -type f -print 2>/dev/null | wc -l) files)"
	@echo "on the machine:  cd /usr/src/kernel && make install && reboot"

disasm: kernel.elf
	$(OBJDUMP) -d kernel.elf

syms: kernel.elf
	@$(NM) -n kernel.elf

clean:
	rm -f kernel.elf kernel.rom kernel.map
	rm -f *.o drivers/*.o fs/*.o net/*.o
	rm -rf .srcstage

distclean: clean
