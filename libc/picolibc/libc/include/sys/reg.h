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
 * sys/reg.h -- picolibc 1.8.12 has none. Linux/m68k's PTRACE_PEEKUSER
 * offsets, in longs, under glibc's names: where each register is in
 * struct user_regs_struct (<sys/user.h>), and the FPU's after it.
 */
#ifndef _SYS_REG_H_
#define _SYS_REG_H_

enum {
    PT_D1 = 0, PT_D2, PT_D3, PT_D4, PT_D5, PT_D6, PT_D7,
    PT_A0, PT_A1, PT_A2, PT_A3, PT_A4, PT_A5, PT_A6,
    PT_D0, PT_USP, PT_ORIG_D0, PT_SR, PT_PC,
    PT_FP0 = 21, PT_FP1 = 24, PT_FP2 = 27, PT_FP3 = 30,
    PT_FP4 = 33, PT_FP5 = 36, PT_FP6 = 39, PT_FP7 = 42,
    PT_FPCR = 45, PT_FPSR, PT_FPIAR
};

#endif /* _SYS_REG_H_ */
