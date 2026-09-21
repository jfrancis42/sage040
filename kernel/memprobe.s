| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| memprobe.s - test one address for memory, and survive it not being there.
|
|   int mem_probe(volatile void *addr, unsigned long pattern);
|
| Returns 1 if the address stored and returned the pattern, 0 if it did
| not or if touching it raised a bus error.  The original contents are
| put back either way.
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
