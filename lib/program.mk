# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# How a program is built, wherever it lives.
#
# Included by system/Makefile and apps/Makefile, which differ only in
# which programs they build and where those end up on the disk. Keeping
# the rules here means the two cannot drift apart -- and they are the
# same kind of thing, built the same way, running under the same rules.
#
# A program carries NO EXTENSION. The kernel decides what is executable
# from the first four bytes of the file, not from its name, because a
# FAT16 volume has no execute permission bit to consult.

M68K_PREFIX ?= $(HOME)/m68k/install
TOOLCHAIN := $(if $(wildcard $(M68K_PREFIX)/bin/m68k-elf-gcc),$(M68K_PREFIX)/bin/m68k-elf,m68k-elf)

CC      := $(TOOLCHAIN)-gcc
SIZE    := $(TOOLCHAIN)-size
OBJDUMP := $(TOOLCHAIN)-objdump
READELF := $(TOOLCHAIN)-readelf

LIB := $(TOPDIR)/lib

# uapi.h is the system call ABI and the only kernel header a program
# gets. Note what is NOT on the include path: ../tests, where the
# machine's hardware header lives. A program cannot reach a chip by
# accident, and if one ever needs to, the include path is the place that
# has to change -- which makes it a decision rather than a slip.
CPUFLAGS := -mcpu=68040
CFLAGS   := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
            -Wall -Wextra -Werror -fno-builtin -fno-stack-protector \
            -ffunction-sections -fdata-sections \
            -I$(LIB) -I$(TOPDIR) -I$(TOPDIR)/kernel
# Every program is compiled with the whole library, so the linker drops
# what a program does not call: a program that never allocates does not
# carry the allocator.
LDFLAGS  := $(CPUFLAGS) -ffreestanding -nostdlib -T $(LIB)/user.ld \
            -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,--gc-sections

# uapi.h and types.h too: they ARE the ABI, and a program built against
# an old copy keeps the old system call numbers -- which is exactly how
# renumbering the socket calls left every program calling the wrong ones
# until something happened to touch its source.
COMMON := $(LIB)/crt0.s $(LIB)/ulib.c $(LIB)/ulib.h $(LIB)/malloc.c \
          $(LIB)/malloc.h $(LIB)/resolv.c $(LIB)/user.ld \
          $(TOPDIR)/kernel/uapi.h $(TOPDIR)/types.h

.PHONY: all install list clean

all: $(PROGS)

# -lgcc LAST, and it belongs there. It is not a library in the usual
# sense: it is the COMPILER'S OWN RUNTIME, the handful of routines gcc
# emits calls to when the 68040 has no instruction for something --
# 64-bit division (__udivdi3, __umoddi3), some floating point, a few
# shifts. A program that never divides a long long never references
# it and pays nothing; `df`, which multiplies clusters by a cluster
# size and so must work in 64 bits to survive a 4 GB volume, could not
# link without it.
#
# It was missing because nothing here had needed it yet, which is not
# the same as nothing here ever needing it.
%: %.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) \
	    -x assembler-with-cpp $(LIB)/crt0.s \
	    -x c $(LIB)/ulib.c $(LIB)/malloc.c $(LIB)/resolv.c $< -lgcc -o $@
	@$(SIZE) $@

# A program keeps its own name. It used to be upper-cased on the way in,
# because FAT16 has no lower case in a short name and `winchtest` became
# WINCHTES; on ext2 a name is just bytes, so `cat` is installed as `cat`
# and typed as `cat`.
install: $(PROGS) $(DISK)
	@if [ -n "$(INSTALL_DIR)" ]; then \
	    $(FSIMG) mkdir $(INSTALL_DIR); \
	 fi
	@for p in $(PROGS); do \
	   $(FSIMG) put -m 755 $$p $(INSTALL_DIR)/$$p; \
	   echo "$$p -> $(DISK) as $(INSTALL_DIR)/$$p"; \
	 done

list: $(DISK)
	@$(FSIMG) ls-l $(INSTALL_DIR)/

disasm-%: %
	$(OBJDUMP) -d $<

clean:
	rm -f $(PROGS)
