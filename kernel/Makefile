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

# Hardware register definitions are shared with the device tests.
SUP := ../tests
CPUFLAGS := -mcpu=68040
CFLAGS := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
 -Wall -Wextra -Werror -fno-builtin -fno-stack-protector \
 -DSAGE040_NO_TESTLIB \
 -I. -I.. -Idrivers -Inet -I$(SUP)
LDFLAGS := $(CPUFLAGS) -ffreestanding -nostdlib -T kernel.ld \
 -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map=kernel.map

# The kernel proper, then the drivers, then the filesystems. Nothing in
# the first group names a chip; everything in the second does, and only
# there.
SRCS := main.c version.c console.c trap.c syscall.c vfs.c dev.c \
 time.c timer.c errno.c string.c exec.c tty.c edit.c \
 pmm.c vm.c mmap.c uaccess.c task.c wait.c signal.c random.c fb.c fbcon.c \
 font8x16.c \
 probe.c shell.c \
 drivers/ns16550.c drivers/ata.c drivers/m48t59.c \
 drivers/mfp.c drivers/sm501.c drivers/i8042.c \
 drivers/smc91c111.c \
 net/net.c net/arp.c net/ip.c net/icmp.c net/udp.c net/dhcp.c net/tcp.c net/socket.c \
 fs/fat16.c

HDRS := kernel.h uapi.h dev.h vfs.h syscall.h console.h errno.h exec.h \
 time.h timer.h tty.h fb.h fbcon.h font.h string.h \
 drivers/drivers.h drivers/rtc.h \
 $(SUP)/sage040.h

.PHONY: all run install boot disasm syms clean distclean

all: kernel.rom

ASMS := start.s memprobe.s taskasm.s

# The layering rule is checked before the link, not documented and hoped
# for. shell.c and edit.c are programs: they reach the system through
# trap #0 and may not include a kernel header that lets them go round it.
# See layercheck.sh for what is allowed and why.
kernel.elf: $(ASMS) $(SRCS) $(HDRS) kernel.ld layercheck.sh
	@./layercheck.sh
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

run: kernel.elf $(DISK)
	$(NET) $(QEMU) $(MACHINE) -no-reboot -kernel kernel.elf \
	 -drive file=$(DISK),format=raw,if=ide \
	 -serial mon:stdio

# Put the kernel on the disk where the boot ROM will find it.
install: kernel.rom $(DISK)
	@mcopy -o -i $(MIMG) kernel.rom ::/KERNEL.ROM
	@echo "KERNEL.ROM: $$(stat -c%s kernel.rom) bytes -> $(DISK) partition 1"

# The real thing: the boot ROM finds KERNEL.ROM on the filesystem and runs it.
boot: install
	$(MAKE) -C ../bootrom boot

disasm: kernel.elf
	$(OBJDUMP) -d kernel.elf

syms: kernel.elf
	@$(NM) -n kernel.elf

clean:
	rm -f kernel.elf kernel.rom kernel.map

distclean: clean
