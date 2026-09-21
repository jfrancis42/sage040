# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# disk.mk - the machine's hard disk, shared by everything that touches it.
#
# The image lives in the project root, not in whichever subdirectory
# happened to create it first: the boot ROM reads it, the kernel reads and
# writes it, and the host puts files on it. It is the machine's disk, not
# any one program's build artifact.
#
# Include it after setting TOPDIR to the path back to the project root:
#
#     TOPDIR := ..
#     include $(TOPDIR)/disk.mk
#
# The layout is a real MS-DOS one, so every host tool below works on the
# plain image file and none of them needs root or a loop device.
#
#   LBA 0       MBR partition table
#   LBA 64      optional raw image, in the boot gap
#   LBA 2048    partition 1, type 0x06, FAT16

TOPDIR ?= .

include $(TOPDIR)/machine.conf

DISK       ?= $(TOPDIR)/hd.img
PART_LBA   ?= 2048
KERNEL_LBA ?= 64
VOLUME     ?= SAGE040

TOTAL_SECTORS := $(shell expr $(DISK_MB) \* 2048)
FS_SECTORS    := $(shell expr $(TOTAL_SECTORS) - $(PART_LBA))
FS_BLOCKS     := $(shell expr $(FS_SECTORS) / 2)
PART_OFFSET   := $(shell expr $(PART_LBA) \* 512)

# mtools reaches into the partition by byte offset.
MIMG := $(DISK)@@$(PART_OFFSET)

.PHONY: disk disk-ls disk-fsck disk-clean

disk: $(DISK)

$(DISK):
	@echo "creating $(DISK_MB) MB disk image $(DISK)"
	@dd if=/dev/zero of=$@ bs=1M count=$(DISK_MB) status=none
	@printf 'label: dos\nunit: sectors\nstart=$(PART_LBA), type=06\n' \
	    | sfdisk -q $@ >/dev/null
	@mkfs.fat -F 16 -n $(VOLUME) --offset $(PART_LBA) $@ $(FS_BLOCKS) >/dev/null
	@echo "partition 1: FAT16 at LBA $(PART_LBA), $(FS_SECTORS) sectors"

# Inspect the disk with the host's own DOS tools.
disk-ls: $(DISK)
	@sfdisk -l $(DISK) 2>/dev/null | tail -3
	@mdir -i $(MIMG) ::/

disk-fsck: $(DISK)
	@fsck.fat -n -v $(MIMG) 2>/dev/null \
	  || echo "(run fsck.fat on the partition offset manually)"

disk-clean:
	rm -f $(DISK)
