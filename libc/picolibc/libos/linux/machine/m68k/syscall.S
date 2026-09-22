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
 * long syscall(long nr, ...)
 *
 * Linux/m68k's convention: the call number in d0, up to five arguments
 * in d1-d5 and a sixth in a0, trap #0, and the result in d0 -- a value
 * from -4095 to -1 being a negated errno, which _syscall_error turns
 * into errno and -1. d2-d5 are callee-saved in the m68k ABI, so they
 * are put back; the kernel preserves everything but d0.
 */

        .text
        .section .text.syscall
        .align  2
        .globl  syscall
        .type   syscall,@function
syscall:
        movem.l %d2-%d5,-(%sp)          | 16 bytes below the arguments
        move.l  20(%sp),%d0             | nr
        move.l  24(%sp),%d1
        move.l  28(%sp),%d2
        move.l  32(%sp),%d3
        move.l  36(%sp),%d4
        move.l  40(%sp),%d5
        move.l  44(%sp),%a0
        trap    #0
        movem.l (%sp)+,%d2-%d5
        cmp.l   #-4095,%d0
        bcs.s   1f                      | below -4095 unsigned: a result
        move.l  %d0,-(%sp)
        jsr     _syscall_error
        addq.l  #4,%sp
1:      rts
        .size   syscall, . - syscall
