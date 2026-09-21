# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Sage040 boot ROM
#
#   make          build bootrom.elf
#   make disk     create the 100 MB disk image
#   make write    write the cube to the disk starting at sector 0
#   make boot     boot the machine: ROM loads sector 0 and runs it
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

MACHINE := -M sage040 -cpu m68040 -m 4

SUP     := ../tests
CPUFLAGS := -mcpu=68040
CFLAGS  := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
           -Wall -Wextra -fno-builtin -fno-stack-protector -I$(SUP)
LDFLAGS := $(CPUFLAGS) -ffreestanding -nostdlib -T bootrom.ld \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

# How much of the disk the ROM reads. Must cover the payload and must stay
# below the boot ROM at 2 MB.
BOOT_SECTORS ?= 256

# The disk the machine boots from: a real MS-DOS disk, so the host can read
# and write it with ordinary tools.
#
#   LBA 0       MBR partition table
#   LBA 64      optional raw image, in the boot gap
#   LBA 2048    partition 1, FAT16, holds KERNEL.ROM
DISK     := hd.img
DISK_MB  := 100
PART_LBA := 2048
KERNEL_LBA := 64
VOLUME   := SAGE040

TOTAL_SECTORS := $(shell expr $(DISK_MB) \* 2048)
FS_SECTORS    := $(shell expr $(TOTAL_SECTORS) - $(PART_LBA))
FS_BLOCKS     := $(shell expr $(FS_SECTORS) / 2)
PART_OFFSET   := $(shell expr $(PART_LBA) \* 512)

# mtools reaches into the partition by byte offset -- no root, no loop mount.
MIMG := $(DISK)@@$(PART_OFFSET)

# What becomes KERNEL.ROM. Swap this for a kernel when there is one.
PAYLOAD_DIR := ../cube
PAYLOAD_ELF := $(PAYLOAD_DIR)/cube.elf
PAYLOAD_BIN := kernel.rom

DEPS := $(SUP)/crt0.s $(SUP)/uart.c $(SUP)/sage040.h bootrom.ld

.PHONY: all disk write write-raw ls fsck boot run disasm clean distclean

all: bootrom.elf

bootrom.elf: bootrom.c $(DEPS)
	$(CC) $(CFLAGS) -DBOOT_SECTORS=$(BOOT_SECTORS) $(LDFLAGS) \
	    -x assembler-with-cpp $(SUP)/crt0.s \
	    -x c $(SUP)/uart.c bootrom.c -o $@
	@$(SIZE) $@

disk: $(DISK)

# A genuine MS-DOS disk: MBR partition table plus a FAT16 filesystem.
# Both steps work on the plain image file; neither needs root.
$(DISK):
	@echo "creating $(DISK_MB) MB disk image $(DISK)"
	@dd if=/dev/zero of=$@ bs=1M count=$(DISK_MB) status=none
	@printf 'label: dos\nunit: sectors\nstart=$(PART_LBA), type=06\n' \
	    | sfdisk -q $@ >/dev/null
	@mkfs.fat -F 16 -n $(VOLUME) --offset $(PART_LBA) $@ $(FS_BLOCKS) >/dev/null
	@echo "partition 1: FAT16 at LBA $(PART_LBA), $(FS_SECTORS) sectors"

# Build the payload, flatten it, and copy it into the filesystem as
# KERNEL.ROM -- which is exactly what you would do by hand with mcopy.
write: $(DISK)
	$(MAKE) -C $(PAYLOAD_DIR) cube.elf
	$(OBJCOPY) -O binary $(PAYLOAD_ELF) $(PAYLOAD_BIN)
	@mcopy -o -i $(MIMG) $(PAYLOAD_BIN) ::/KERNEL.ROM
	@echo "KERNEL.ROM: $$(stat -c%s $(PAYLOAD_BIN)) bytes -> $(DISK) partition 1"
	@mdir -i $(MIMG) ::/ | sed 's/^/  /'

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

# Inspect the disk with the host's own DOS tools.
ls: $(DISK)
	@sfdisk -l $(DISK) 2>/dev/null | tail -3
	@mdir -i $(MIMG) ::/

fsck: $(DISK)
	@fsck.fat -n -v $(DISK)@@$(PART_OFFSET) 2>/dev/null \
	  || fsck.fat -n $(DISK) 2>/dev/null \
	  || echo "(run fsck.fat on the partition offset manually)"

# Boot the machine. The ROM is loaded by -kernel; everything after that
# comes off the disk.
boot: bootrom.elf $(DISK)
	$(QEMU) $(MACHINE) -kernel bootrom.elf \
	    -drive file=$(DISK),format=raw,if=ide \
	    -serial stdio

run: boot

disasm: bootrom.elf
	$(OBJDUMP) -d bootrom.elf

clean:
	rm -f bootrom.elf $(PAYLOAD_BIN)

# Also removes the disk image.
distclean: clean
	rm -f $(DISK)
