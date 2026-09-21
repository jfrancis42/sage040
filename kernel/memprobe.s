| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| memprobe.s - touch an address that may not answer, and survive it.
|
|   int mem_probe(volatile void *addr, unsigned long pattern);
|   int io_probe8(volatile void *addr);
|   int io_probe16(volatile void *addr);
|   int io_probe32(volatile void *addr);
|
| mem_probe returns 1 if the address stored and returned the pattern, 0
| if it did not or if touching it raised a bus error.  The original
| contents are put back either way.
|
| io_probe8 and io_probe32 return 1 if the address could be READ without
| a bus error, and nothing else.  They are how a driver asks whether its
| chip is fitted.  They never write: an unknown address is not somewhere
| to put a test pattern, because if something IS there the write may mean
| something.  And they never look at the value, because an absent device
| and a device holding zero are indistinguishable by value -- the whole
| question is whether the access completed.
|
| Why a driver needs this at all: QEMU faults on an address with no
| device behind it, exactly as a real board faults on an empty socket.
| Without this, running a kernel on an emulator built before one of its
| devices existed does not report a missing device, it panics in the
| first driver that reaches for one -- which reads as a kernel bug and is
| not.
|
| Sizing memory by walking off the end of it is the traditional 68k ROM
| trick and still the only way to do it here: nothing on this machine
| reports how much RAM is fitted, and QEMU faults on the first address
| past the end just as real hardware with no card in the slot would.
|
| The recovery is deliberately blunt.  The handler throws the exception
| frame away, restores the stack pointer to what it was when mem_probe()
| was entered, and returns to mem_probe's caller -- so the faulting
| instruction is never retried and nothing is resumed.  That is only safe
| because this routine touches no callee-saved register and holds no
| state worth unwinding; it is not a general fault handler and should not
| grow into one.
|
| A 68040 can report a write fault late, after the store has retired,
| because writes go through a buffer.  The nops force the pipeline to
| settle so the fault is attributed to the access that caused it.

        .text
        .globl  mem_probe
        .type   mem_probe,@function
mem_probe:
        move.l  %sp,probe_sp            | SP here points at the return address
        move.l  4(%sp),%a0              | addr
        move.l  8(%sp),%d1              | pattern

        movec   %vbr,%a1
        move.l  8(%a1),probe_saved      | vector 2 is bus error
        move.l  #probe_berr,8(%a1)
        nop

        move.l  (%a0),%d0               | original contents; may fault
        move.l  %d1,(%a0)               | may fault
        nop
        cmp.l   (%a0),%d1               | did it stick?
        bne.s   1f

        move.l  %d0,(%a0)               | put it back
        moveq   #1,%d0
        bra.s   2f
1:      move.l  %d0,(%a0)
        moveq   #0,%d0
2:
        movec   %vbr,%a1
        move.l  probe_saved,8(%a1)
        rts

|
| int io_probe8(volatile void *addr)  - can this address be read at all?
| int io_probe32(volatile void *addr)
|
| Same recovery as mem_probe and the same caveats: no callee-saved
| register is touched and no state is held, so throwing the frame away
| and returning is safe here and would not be anywhere else.
|
| The width matters.  Several device regions declare a minimum access
| size and a too-narrow read lands in the wrong byte lane -- silently,
| with no fault -- so a driver probes at the width it will actually use.
| SM501 registers are 32-bit only; the MFP, the UART and the 8042 are
| byte registers.
|
        .globl  io_probe8
        .type   io_probe8,@function
io_probe8:
        move.l  %sp,probe_sp
        move.l  4(%sp),%a0
        bsr.s   probe_arm
        moveq   #0,%d0
        move.b  (%a0),%d0               | may fault
        nop
        bra.s   probe_ok

        .globl  io_probe16
        .type   io_probe16,@function
io_probe16:
        move.l  %sp,probe_sp
        move.l  4(%sp),%a0
        bsr.s   probe_arm
        moveq   #0,%d0
        move.w  (%a0),%d0               | may fault
        nop
        bra.s   probe_ok

        .globl  io_probe32
        .type   io_probe32,@function
io_probe32:
        move.l  %sp,probe_sp
        move.l  4(%sp),%a0
        bsr.s   probe_arm
        move.l  (%a0),%d0               | may fault
        nop
        bra.s   probe_ok

| Install the bus error vector, keeping the old one.  Called with bsr, so
| it must not disturb a0.
probe_arm:
        movec   %vbr,%a1
        move.l  8(%a1),probe_saved
        move.l  #probe_berr,8(%a1)
        nop
        rts

probe_ok:
        movec   %vbr,%a1
        move.l  probe_saved,8(%a1)
        moveq   #1,%d0
        rts

|
| Bus error during the probe: abandon the frame and leave as if
| mem_probe() had simply returned 0.
|
probe_berr:
        movea.l probe_sp,%sp
        movec   %vbr,%a1
        move.l  probe_saved,8(%a1)
        moveq   #0,%d0
        rts

        .bss
        .align  4
probe_sp:
        .space  4
probe_saved:
        .space  4
