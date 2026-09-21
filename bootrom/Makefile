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

# The 100 MB disk the machine boots from.
DISK    := hd.img
DISK_MB := 100

# What gets written to sector 0. Swap this for a kernel later.
PAYLOAD_DIR := ../cube
PAYLOAD_ELF := $(PAYLOAD_DIR)/cube.elf
PAYLOAD_BIN := payload.bin

DEPS := $(SUP)/crt0.s $(SUP)/uart.c $(SUP)/sage040.h bootrom.ld

.PHONY: all disk write boot run disasm clean distclean

all: bootrom.elf

bootrom.elf: bootrom.c $(DEPS)
	$(CC) $(CFLAGS) -DBOOT_SECTORS=$(BOOT_SECTORS) $(LDFLAGS) \
	    -x assembler-with-cpp $(SUP)/crt0.s \
	    -x c $(SUP)/uart.c bootrom.c -o $@
	@$(SIZE) $@

disk: $(DISK)

$(DISK):
	@echo "creating $(DISK_MB) MB disk image $(DISK)"
	@dd if=/dev/zero of=$(DISK) bs=1M count=$(DISK_MB) status=none

# Build the payload, flatten it, and lay it down from sector 0.
# conv=notrunc keeps the rest of the 100 MB image intact.
write: $(DISK)
	$(MAKE) -C $(PAYLOAD_DIR) cube.elf
	$(OBJCOPY) -O binary $(PAYLOAD_ELF) $(PAYLOAD_BIN)
	@sz=$$(stat -c%s $(PAYLOAD_BIN)); \
	 sec=$$(( (sz + 511) / 512 )); \
	 if [ $$sec -gt $(BOOT_SECTORS) ]; then \
	   echo "payload is $$sec sectors but the ROM only reads $(BOOT_SECTORS);"; \
	   echo "rebuild with: make BOOT_SECTORS=$$sec"; exit 1; \
	 fi; \
	 echo "writing $$sz bytes ($$sec sectors) to $(DISK) at sector 0"
	@dd if=$(PAYLOAD_BIN) of=$(DISK) bs=512 conv=notrunc status=none

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
