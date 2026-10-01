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
 * sys/user.h -- picolibc 1.8.12 has none. Linux/m68k's register
 * layouts, as PTRACE_GETREGS and PTRACE_GETFPREGS hand them over, with
 * glibc's names. PTRACE_PEEKUSER's offsets are into struct
 * user_regs_struct: d1 is 0, sr 17 * 4, the pc 18 * 4.
 */
#ifndef _SYS_USER_H_
#define _SYS_USER_H_

struct user_m68kfp_struct {
    int fpregs[24];             /* fp0-fp7, twelve bytes each       */
    int fpcntl[3];              /* fpcr, fpsr, fpiar                */
};

struct user_regs_struct {
    long d1, d2, d3, d4, d5, d6, d7;
    long a0, a1, a2, a3, a4, a5, a6;
    long d0;
    long usp;
    long orig_d0;
    short stkadj;
    short sr;
    long pc;
    short fmtvec;
    short __fill;
};

struct user {
    struct user_regs_struct regs;
    int u_fpvalid;
    struct user_m68kfp_struct m68kfp;
    int u_tsize, u_dsize, u_ssize;
    unsigned long start_code, start_stack;
    long int signal;
    int reserved;
    struct user_regs_struct *u_ar0;
    struct user_m68kfp_struct *u_fpstate;
    unsigned long magic;
    char u_comm[32];
};

#define NBPG            4096
#define UPAGES          1
#define HOST_TEXT_START_ADDR    (u.start_code)
#define HOST_STACK_END_ADDR     (u.start_stack + u.u_ssize * NBPG)

#endif /* _SYS_USER_H_ */
