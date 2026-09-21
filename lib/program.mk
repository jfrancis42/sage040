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

COMMON := $(LIB)/crt0.s $(LIB)/ulib.c $(LIB)/ulib.h $(LIB)/malloc.c \
          $(LIB)/malloc.h $(LIB)/user.ld

.PHONY: all install list clean

all: $(PROGS)

%: %.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) \
	    -x assembler-with-cpp $(LIB)/crt0.s \
	    -x c $(LIB)/ulib.c $(LIB)/malloc.c $< -o $@
	@$(SIZE) $@

install: $(PROGS) $(DISK)
	@if [ -n "$(INSTALL_DIR)" ]; then \
	    mmd -i $(MIMG) ::$(INSTALL_DIR) 2>/dev/null || true; \
	 fi
	@for p in $(PROGS); do \
	   mcopy -o -i $(MIMG) $$p ::$(INSTALL_DIR)/$$(echo $$p | tr a-z A-Z); \
	   echo "$$p -> $(DISK) as $(INSTALL_DIR)/$$(echo $$p | tr a-z A-Z)"; \
	 done

list: $(DISK)
	@mdir -i $(MIMG) ::$(INSTALL_DIR)/

disasm-%: %
	$(OBJDUMP) -d $<

clean:
	rm -f $(PROGS)
