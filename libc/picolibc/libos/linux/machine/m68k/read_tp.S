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
 * The two functions gcc calls for thread-local storage on m68k.
 *
 * BOTH RETURN IN %a0. gcc emits these calls itself (m68k.c,
 * m68k_call_m68k_read_tp and m68k_call_tls_get_addr) with its libcall
 * value in a0, where an ordinary function here returns in d0 -- so
 * neither can be a plain C function. Both set d0 too, for C callers.
 *
 * __m68k_read_tp: the thread pointer. The 68040 has no register for it;
 * the kernel keeps it per task (get_thread_area, Linux/m68k's), so this
 * is a system call, as it is in glibc on m68k. Only d0 and a0 change.
 *
 * __tls_get_addr: the address of a TLS variable, given its module and
 * offset -- the general-dynamic access, which is what code built -fPIC
 * uses. The work is in tls.c.
 */
        .text
        .align  2
        .globl  __m68k_read_tp
        .type   __m68k_read_tp,@function
__m68k_read_tp:
        move.l  #333,%d0                | __NR_get_thread_area
        trap    #0
        move.l  %d0,%a0
        rts
        .size   __m68k_read_tp, . - __m68k_read_tp

        .globl  __tls_get_addr
        .type   __tls_get_addr,@function
__tls_get_addr:
        move.l  4(%sp),-(%sp)           | the tls_index
        bsr.l   __sage040_tls_get_addr  | hidden: a direct call, PIC or not
        addq.l  #4,%sp
        move.l  %d0,%a0
        rts
        .size   __tls_get_addr, . - __tls_get_addr

        .section .note.GNU-stack,"",@progbits
