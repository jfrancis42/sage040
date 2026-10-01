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
 * sys/ptrace.h -- picolibc 1.8.12 has none. glibc's interface: the
 * requests Linux/m68k has, by Linux's numbers, and ptrace() as glibc
 * declares it -- variadic, with a PEEK request's word as its result and
 * errno telling a word of -1 from a failure. The kernel side is
 * kernel/ptrace.c.
 */
#ifndef _SYS_PTRACE_H_
#define _SYS_PTRACE_H_

#include <sys/cdefs.h>
#include <sys/types.h>

_BEGIN_STD_C

enum __ptrace_request {
    PTRACE_TRACEME = 0,
    PTRACE_PEEKTEXT = 1,
    PTRACE_PEEKDATA = 2,
    PTRACE_PEEKUSER = 3,
    PTRACE_POKETEXT = 4,
    PTRACE_POKEDATA = 5,
    PTRACE_POKEUSER = 6,
    PTRACE_CONT = 7,
    PTRACE_KILL = 8,
    PTRACE_SINGLESTEP = 9,
    PTRACE_GETREGS = 12,
    PTRACE_SETREGS = 13,
    PTRACE_GETFPREGS = 14,
    PTRACE_SETFPREGS = 15,
    PTRACE_ATTACH = 16,
    PTRACE_DETACH = 17,
    PTRACE_SYSCALL = 24,
    PTRACE_SETOPTIONS = 0x4200,
    PTRACE_GETEVENTMSG = 0x4201,
    PTRACE_GETSIGINFO = 0x4202,
    PTRACE_SETSIGINFO = 0x4203
};
#define PT_TRACE_ME     PTRACE_TRACEME
#define PT_READ_I       PTRACE_PEEKTEXT
#define PT_READ_D       PTRACE_PEEKDATA
#define PT_READ_U       PTRACE_PEEKUSER
#define PT_WRITE_I      PTRACE_POKETEXT
#define PT_WRITE_D      PTRACE_POKEDATA
#define PT_WRITE_U      PTRACE_POKEUSER
#define PT_CONTINUE     PTRACE_CONT
#define PT_KILL         PTRACE_KILL
#define PT_STEP         PTRACE_SINGLESTEP
#define PT_GETREGS      PTRACE_GETREGS
#define PT_SETREGS      PTRACE_SETREGS
#define PT_GETFPREGS    PTRACE_GETFPREGS
#define PT_SETFPREGS    PTRACE_SETFPREGS
#define PT_ATTACH       PTRACE_ATTACH
#define PT_DETACH       PTRACE_DETACH
#define PT_SYSCALL      PTRACE_SYSCALL
#define PT_SETOPTIONS   PTRACE_SETOPTIONS
#define PT_GETEVENTMSG  PTRACE_GETEVENTMSG
#define PT_GETSIGINFO   PTRACE_GETSIGINFO
#define PT_SETSIGINFO   PTRACE_SETSIGINFO

/* PTRACE_SETOPTIONS */
#define PTRACE_O_TRACESYSGOOD   0x00000001
#define PTRACE_O_TRACEFORK      0x00000002
#define PTRACE_O_TRACEVFORK     0x00000004
#define PTRACE_O_TRACECLONE     0x00000008
#define PTRACE_O_TRACEEXEC      0x00000010
#define PTRACE_O_TRACEVFORKDONE 0x00000020
#define PTRACE_O_TRACEEXIT      0x00000040
#define PTRACE_O_EXITKILL       0x00100000
#define PTRACE_O_MASK           0x0010007f

/* The event in bits 16-23 of a stop's status. */
#define PTRACE_EVENT_FORK       1
#define PTRACE_EVENT_VFORK      2
#define PTRACE_EVENT_CLONE      3
#define PTRACE_EVENT_EXEC       4
#define PTRACE_EVENT_VFORK_DONE 5
#define PTRACE_EVENT_EXIT       6

long ptrace(int __request, ...);

_END_STD_C

#endif /* _SYS_PTRACE_H_ */
