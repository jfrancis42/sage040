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
 * sys/procfs.h -- picolibc 1.8.12 has none. The register-set types a
 * debugger and a core file share, as glibc defines them for m68k: a
 * general register set is struct user_regs_struct read as an array of
 * longs, a floating-point one is struct user_m68kfp_struct.
 */
#ifndef _SYS_PROCFS_H_
#define _SYS_PROCFS_H_

#include <sys/types.h>
#include <sys/time.h>
#include <sys/user.h>
#include <signal.h>

typedef unsigned long elf_greg_t;
#define ELF_NGREG (sizeof(struct user_regs_struct) / sizeof(elf_greg_t))
typedef elf_greg_t elf_gregset_t[ELF_NGREG];
typedef struct user_m68kfp_struct elf_fpregset_t;

struct elf_siginfo {
    int si_signo;
    int si_code;
    int si_errno;
};

struct elf_prstatus {
    struct elf_siginfo pr_info;
    short int pr_cursig;
    unsigned long int pr_sigpend;
    unsigned long int pr_sighold;
    pid_t pr_pid, pr_ppid, pr_pgrp, pr_sid;
    struct timeval pr_utime, pr_stime, pr_cutime, pr_cstime;
    elf_gregset_t pr_reg;
    int pr_fpvalid;
};

#define ELF_PRARGSZ 80
struct elf_prpsinfo {
    char pr_state, pr_sname, pr_zomb, pr_nice;
    unsigned long int pr_flag;
    unsigned short int pr_uid, pr_gid;
    int pr_pid, pr_ppid, pr_pgrp, pr_sid;
    char pr_fname[16];
    char pr_psargs[ELF_PRARGSZ];
};

typedef void *psaddr_t;
typedef elf_gregset_t prgregset_t;
typedef elf_fpregset_t prfpregset_t;
typedef pid_t lwpid_t;
typedef struct elf_prstatus prstatus_t;
typedef struct elf_prpsinfo prpsinfo_t;

#endif /* _SYS_PROCFS_H_ */
