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
# The filesystem is ext2 and the layout is an ordinary PC one, so every
# host tool below works on the plain image file and none of them needs
# root or a loop device.
#
#   LBA 0       MBR partition table
#   LBA 64      optional raw kernel image, in the boot gap
#   LBA 2048    partition 1, type 0x83, ext2
#
# Reaching the filesystem inside the image is e2fsprogs' "?offset="
# suffix, and tools/fsimg.sh is the one place that knows it. Use
# $(FSIMG) rather than calling debugfs: a Makefile that spells out its
# own offset is a Makefile that breaks when the layout moves.

TOPDIR ?= .

include $(TOPDIR)/machine.conf

DISK       ?= $(TOPDIR)/hd.img
PART_LBA   ?= 2048
KERNEL_LBA ?= 64
VOLUME     ?= SAGE040

# 4 KB blocks. Twelve direct pointers then reach 48 KB and one indirect
# block reaches 4 MB, which is why the boot ROM needs no double
# indirection to load a kernel; at 1 KB it would.
FS_BLOCK_SIZE ?= 4096

TOTAL_SECTORS := $(shell expr $(DISK_MB) \* 2048)
FS_SECTORS    := $(shell expr $(TOTAL_SECTORS) - $(PART_LBA))
PART_OFFSET   := $(shell expr $(PART_LBA) \* 512)

# Everything that touches the filesystem goes through this.
FSIMG := PART_OFFSET=$(PART_OFFSET) FS_BLOCK_SIZE=$(FS_BLOCK_SIZE) \
         $(TOPDIR)/tools/fsimg.sh $(DISK)

# The same thing in absolute terms, for a recipe that has cd'd into a
# build directory: $(FSIMG) is relative to the project root and names
# nothing once the shell has moved.
ABS_FSIMG := PART_OFFSET=$(PART_OFFSET) FS_BLOCK_SIZE=$(FS_BLOCK_SIZE) \
         $(abspath $(TOPDIR)/tools/fsimg.sh) $(abspath $(DISK))

.PHONY: disk disk-ls disk-fsck disk-clean

disk: $(DISK)

$(DISK):
	@echo "creating $(DISK_MB) MB disk image $(DISK)"
	@dd if=/dev/zero of=$@ bs=1M count=$(DISK_MB) status=none
	@printf 'label: dos\nunit: sectors\nstart=$(PART_LBA), type=83\n' \
	    | sfdisk -q $@ >/dev/null
	@$(FSIMG) mkfs $(VOLUME)
	@echo "partition 1: ext2 at LBA $(PART_LBA), $(FS_SECTORS) sectors"

# Inspect the disk with the host's own tools.
disk-ls: $(DISK)
	@sfdisk -l $(DISK) 2>/dev/null | tail -3
	@$(FSIMG) ls-l /

disk-fsck: $(DISK)
	@$(FSIMG) fsck

disk-clean:
	rm -f $(DISK)
