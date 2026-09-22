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
 * Linux/m68k defines no SA_RESTORER and ignores sa_restorer: the kernel
 * writes a trampoline into each signal frame and returns through that,
 * making the right sigreturn for the kind of frame it built. So
 * LINUX_SA_RESTORER is 0 in linux-signal.h and these are never reached.
 * They exist because sigaction.c takes their addresses, and they make
 * the call they would have to make if they were.
 */

#include <linux/linux-syscall.h>

        .text
        .section .text.__sa_restore
        .align  2
        .globl  __sa_restore
        .type   __sa_restore,@function
__sa_restore:
        move.l  #LINUX_SYS_sigreturn,%d0
        trap    #0
        .size   __sa_restore, . - __sa_restore

        .globl  __restore_rt
        .type   __restore_rt,@function
__restore_rt:
        move.l  #LINUX_SYS_rt_sigreturn,%d0
        trap    #0
        .size   __restore_rt, . - __restore_rt
