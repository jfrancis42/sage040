# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Sage040 boot ROM
#
#   make          build bootrom.elf
#   make disk     create the 100 MB MS-DOS disk image, in the project root
#   make write    build the kernel and put it on the disk as KERNEL.ROM
#   make boot     boot the machine: ROM finds KERNEL.ROM and runs it
#   make clean    remove build artifacts (keeps the disk image)

# Toolchain and emulator locations.
#
# Both default to the local build described in the README, and fall back to
# whatever is on PATH if that is not present. Override either on the command
# line, e.g.  make M68K_PREFIX=/opt/m68k  or  make QEMU=/path/to/qemu-system-m68k
M68K_PREFIX ?= $(HOME)/m68k/install
SAGE_QEMU   ?= $(HOME)/m68k/sage040-qemu

TOOLCHAIN := $(if $(wildcard $(M68K_PREFIX)/bin/m68k-elf-gcc),$(M68K_PREFIX)/bin/m68k-elf,m68k-elf)
QEMU      ?= $(if $(wildcard $(SAGE_QEMU)/bin/qemu-system-m68k),$(SAGE_QEMU)/bin/qemu-system-m68k,qemu-system-m68k)

CC      := $(TOOLCHAIN)-gcc
OBJCOPY := $(TOOLCHAIN)-objcopy
OBJDUMP := $(TOOLCHAIN)-objdump
SIZE    := $(TOOLCHAIN)-size

TOPDIR := ..

# SPEED / ICOUNT: shared with kernel and cube. AFTER TOPDIR, which it
# uses -- an include above that expands to /speed.mk and silently
# includes nothing.
include $(TOPDIR)/speed.mk
include $(TOPDIR)/machine.conf

MACHINE := -M sage040 -cpu m68040 -m $(RAM_MB)

SUP     := ../tests
CPUFLAGS := -mcpu=68040
CFLAGS  := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
           -Wall -Wextra -fno-builtin -fno-stack-protector -I.. -I$(SUP)
LDFLAGS := $(CPUFLAGS) -ffreestanding -nostdlib -T bootrom.ld \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

# How much of the disk the ROM reads. Must cover the payload and must stay
# below the boot ROM at 2 MB.
BOOT_SECTORS ?= 1920      # 960 KB: see bootrom.c

# The disk the machine boots from: a real MS-DOS disk in the project root,
# shared with the kernel and with the host. See ../disk.mk.
include $(TOPDIR)/disk.mk

# disk.mk defines a target before this file does, so say which is the
# default rather than letting include order decide it.
.DEFAULT_GOAL := all

# What the raw-image fallback carries. The filesystem path gets KERNEL.ROM
# from the kernel's own Makefile.
PAYLOAD_DIR := ../cube
PAYLOAD_ELF := $(PAYLOAD_DIR)/cube.elf
PAYLOAD_BIN := payload.bin

DEPS := $(SUP)/crt0.s $(SUP)/uart.c $(SUP)/sage040.h bootrom.ld

.PHONY: all write write-cube write-raw ls fsck boot run disasm clean distclean

all: bootrom.elf

bootrom.elf: bootrom.c $(DEPS)
	$(CC) $(CFLAGS) -DBOOT_SECTORS=$(BOOT_SECTORS) $(LDFLAGS) \
	    -x assembler-with-cpp $(SUP)/crt0.s \
	    -x c $(SUP)/uart.c bootrom.c -o $@
	@$(SIZE) $@

# The kernel owns the file it installs, so ask it to.
write: $(DISK)
	$(MAKE) -C ../kernel install
	@$(FSIMG) ls-l / | sed 's/^/  /'

# The cube in place of the kernel -- the demonstration this ROM was first
# written against, and a useful way to prove the loader without the kernel
# in the picture.
write-cube: $(DISK)
	$(MAKE) -C $(PAYLOAD_DIR) cube.elf
	$(OBJCOPY) -O binary $(PAYLOAD_ELF) $(PAYLOAD_BIN)
	@$(FSIMG) put $(PAYLOAD_BIN) /KERNEL.ROM
	@echo "KERNEL.ROM: $$(stat -c%s $(PAYLOAD_BIN)) bytes (cube) -> $(DISK)"
	@$(FSIMG) ls-l / | sed 's/^/  /'

# The fallback path: a raw image in the boot gap, no filesystem involved.
write-raw: $(DISK)
	$(MAKE) -C $(PAYLOAD_DIR) cube.elf
	$(OBJCOPY) -O binary $(PAYLOAD_ELF) $(PAYLOAD_BIN)
	@sz=$$(stat -c%s $(PAYLOAD_BIN)); \
	 sec=$$(( (sz + 511) / 512 )); \
	 if [ $$(( $(KERNEL_LBA) + $$sec )) -gt $(PART_LBA) ]; then \
	   echo "payload would run into the filesystem at LBA $(PART_LBA)"; exit 1; \
	 fi; \
	 if [ $$sec -gt $(BOOT_SECTORS) ]; then \
	   echo "payload is $$sec sectors but the ROM reads $(BOOT_SECTORS);"; \
	   echo "rebuild with: make BOOT_SECTORS=$$sec"; exit 1; \
	 fi; \
	 echo "writing $$sz bytes ($$sec sectors) at LBA $(KERNEL_LBA)"
	@dd if=$(PAYLOAD_BIN) of=$(DISK) bs=512 seek=$(KERNEL_LBA) \
	    conv=notrunc status=none

# Inspect the disk with the host's own DOS tools (defined in ../disk.mk).
ls: disk-ls

fsck: disk-fsck

# Boot the machine. The ROM is loaded by -kernel; everything after that
# comes off the disk.
boot: bootrom.elf $(DISK)
	../tools/qemu-net.sh $(QEMU) $(MACHINE) $(ICOUNT) -no-reboot -kernel bootrom.elf \
	    -drive file=$(DISK),format=raw,if=ide \
	    -serial stdio

run: boot

disasm: bootrom.elf
	$(OBJDUMP) -d bootrom.elf

clean:
	rm -f bootrom.elf $(PAYLOAD_BIN)

# Also removes the disk image, which lives in the project root.
distclean: clean disk-clean
