/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright © 2026 Jeff Francis
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * int __clone(int (*fn)(void *), void *stack_top, int flags, void *arg,
 *             int *ptid, int *ctid);
 *
 * WHY THIS IS ASSEMBLY. clone() returns twice, like fork, but the second
 * return is on a DIFFERENT STACK -- the one being handed to the new
 * thread. The C the compiler generates around the call assumes its
 * frame is still there; in the new thread it is not, and the function
 * it would return through never ran. So the child's side of the trap
 * has to be written by hand, and the very first thing it does is call
 * the function it was given and then exit. It never returns.
 *
 * The arguments the child needs -- fn and arg -- are pushed ONTO THE
 * CHILD'S OWN STACK before the trap, because registers are the only
 * other way to carry them across and the kernel zeroes none of them but
 * d0. Putting them on that stack is also what makes the child's first
 * instruction able to find them with nothing but its own stack pointer.
 */

        .text
        .align  2
        .globl  __clone
        .type   __clone,@function
__clone:
        movem.l %d2-%d5,-(%sp)          | 16 bytes below the arguments
        move.l  20(%sp),%d1             | fn
        move.l  24(%sp),%a1             | stack_top
        move.l  28(%sp),%d2             | flags (clone's first argument)
        move.l  32(%sp),%d0             | arg
        move.l  36(%sp),%d3             | ptid
        move.l  40(%sp),%d4             | ctid

        | Refuse a null fn or stack here rather than in the kernel: the
        | child would have nothing to run or nowhere to run it.
        tst.l   %d1
        beq.s   .Linval
        move.l  %a1,%d5
        beq.s   .Linval

        | The child's stack: arg, then fn, so that after the trap the
        | child pops fn into a0 and leaves arg where a call wants it.
        sub.l   #8,%a1
        move.l  %d0,4(%a1)              | arg
        move.l  %d1,(%a1)               | fn

        move.l  #120,%d0                | __NR_clone
        move.l  %d2,%d1                 | flags
        move.l  %a1,%d2                 | child stack
        move.l  %d3,%d3                 | ptid (already there)
        move.l  %d4,%d4                 | ctid (already there)
        sub.l   %d5,%d5                 | tls: none, see pthread.c
        trap    #0

        tst.l   %d0
        beq.s   .Lchild                 | 0 means this is the new thread

        | The parent: the child's tid, or a negated errno.
        movem.l (%sp)+,%d2-%d5
        cmp.l   #-4095,%d0
        bcs.s   1f
        move.l  %d0,-(%sp)
#ifdef __PIC__
        bsr.l   _syscall_error@PLTPC
#else
        jsr     _syscall_error
#endif
        addq.l  #4,%sp
1:      rts

.Linval:
        movem.l (%sp)+,%d2-%d5
        move.l  #-22,%d0                | -EINVAL
        move.l  %d0,-(%sp)
#ifdef __PIC__
        bsr.l   _syscall_error@PLTPC
#else
        jsr     _syscall_error
#endif
        addq.l  #4,%sp
        rts

        /*
         * The child. Its stack holds fn and arg and nothing else: there
         * is no return address under them, which is deliberate -- if fn
         * ever returned into nowhere the fault would be silent, so the
         * exit below is the only way out and the trap after it is the
         * proof of that.
         */
.Lchild:
        move.l  (%sp)+,%a0              | fn
        jsr     (%a0)                   | arg is already at 0(%sp)
        addq.l  #4,%sp
        move.l  %d0,%d1                 | its return value is the status
        move.l  #1,%d0                  | __NR_exit: THIS THREAD only
        trap    #0
        illegal                         | exit does not return
        .size   __clone, . - __clone
