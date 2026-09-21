# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
# Makefile - Sage040 device tests
#
#   make            build every test ELF
#   make run        build and run all of them, printing results
#   make run-t3     run one test
#   make disasm-t1  disassemble one test

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
OBJCOPY   := $(TOOLCHAIN)-objcopy
SIZE      := $(TOOLCHAIN)-size

MACHINE   := -M sage040 -cpu m68040 -m 4

CPUFLAGS  := -mcpu=68040
CFLAGS    := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
             -Wall -Wextra -fno-builtin -fno-stack-protector -I. -I..
LDFLAGS   := $(CPUFLAGS) -ffreestanding -nostdlib -T sage040.ld \
             -Wl,--build-id=none

TESTS  := t1-cpu t2-uart t3-ata t4-net t5-mmu t6-irq \
          t7-mfp-irq t8-mfp-timers t9-mfp-usart t10-sm501 t11-rtc
ELFS   := $(addsuffix .elf,$(TESTS))
COMMON := crt0.s uart.c mfp.c sage040.h sage040.ld

DISK   := disk.img

.PHONY: all run clean $(addprefix run-,$(TESTS))

all: $(ELFS)

%.elf: %.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -x assembler-with-cpp crt0.s -x c uart.c mfp.c $< -o $@
	@$(SIZE) $@

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=8 status=none

define RUNTEST
run-$(1): $(1).elf $(DISK)
	@./runtest.sh $(1)
endef
$(foreach t,$(TESTS),$(eval $(call RUNTEST,$(t))))

run: $(ELFS) $(DISK)
	@pass=0; fail=0; \
	for t in $(TESTS); do \
	  if ./runtest.sh $$t; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo ""; \
	echo "==================================="; \
	echo " test programs passed: $$pass"; \
	echo " test programs failed: $$fail"; \
	echo "==================================="; \
	test $$fail -eq 0

disasm-%: %.elf
	$(OBJDUMP) -d $<

clean:
	rm -f *.elf *.out $(DISK)
