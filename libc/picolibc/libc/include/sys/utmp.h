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
 * sys/utmp.h -- picolibc 1.8.12 installs a <utmp.h> that includes this
 * and does not provide it, so anything that included <utmp.h> did not
 * compile. The record is Linux/m68k's, for programs that size a buffer
 * from UT_NAMESIZE and the like. There are no utmp FUNCTIONS: the
 * system keeps no utmp file (who(1) and w(1) read the kernel's list of
 * sessions instead), so getutent and friends are deliberately absent.
 */
#ifndef _SYS_UTMP_H_
#define _SYS_UTMP_H_

#include <sys/types.h>
#include <stdint.h>

#define UT_LINESIZE     32
#define UT_NAMESIZE     32
#define UT_HOSTSIZE     256

#define EMPTY           0
#define RUN_LVL         1
#define BOOT_TIME       2
#define NEW_TIME        3
#define OLD_TIME        4
#define INIT_PROCESS    5
#define LOGIN_PROCESS   6
#define USER_PROCESS    7
#define DEAD_PROCESS    8
#define ACCOUNTING      9

struct exit_status {
    short e_termination;
    short e_exit;
};

struct utmp {
    short ut_type;
    pid_t ut_pid;
    char ut_line[UT_LINESIZE];
    char ut_id[4];
    char ut_user[UT_NAMESIZE];
    char ut_host[UT_HOSTSIZE];
    struct exit_status ut_exit;
    int32_t ut_session;
    struct {
        int32_t tv_sec;
        int32_t tv_usec;
    } ut_tv;
    int32_t ut_addr_v6[4];
    char __reserved[20];
};

#define ut_name ut_user
#define ut_time ut_tv.tv_sec

#define _PATH_UTMP      "/var/run/utmp"
#define _PATH_WTMP      "/var/log/wtmp"
#define UTMP_FILE       _PATH_UTMP
#define UTMP_FILENAME   _PATH_UTMP
#define WTMP_FILE       _PATH_WTMP
#define WTMP_FILENAME   _PATH_WTMP

/* glibc's interface to it: libos/linux/machine/m68k/utmp.c */
int utmpname(const char *file);
void setutent(void);
void endutent(void);
struct utmp *getutent(void);
struct utmp *getutid(const struct utmp *ut);
struct utmp *getutline(const struct utmp *ut);
struct utmp *pututline(const struct utmp *ut);
void updwtmp(const char *file, const struct utmp *ut);
int login_tty(int fd);           /* posix-extra.c */

#endif /* _SYS_UTMP_H_ */
