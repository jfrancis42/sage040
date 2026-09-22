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
 * syslog.h -- picolibc 1.8.12 has none. POSIX's interface, with the
 * values every Unix uses. The implementation is in
 * libos/linux/machine/m68k/syslog.c.
 */
#ifndef _SYSLOG_H_
#define _SYSLOG_H_

#include <sys/cdefs.h>
#include <stdarg.h>

_BEGIN_STD_C

/* Priorities. */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#define LOG_PRIMASK 0x07
#define LOG_PRI(p)  ((p) & LOG_PRIMASK)

/* Facilities. */
#define LOG_KERN     (0 << 3)
#define LOG_USER     (1 << 3)
#define LOG_MAIL     (2 << 3)
#define LOG_DAEMON   (3 << 3)
#define LOG_AUTH     (4 << 3)
#define LOG_SYSLOG   (5 << 3)
#define LOG_LPR      (6 << 3)
#define LOG_NEWS     (7 << 3)
#define LOG_UUCP     (8 << 3)
#define LOG_CRON     (9 << 3)
#define LOG_AUTHPRIV (10 << 3)
#define LOG_FTP      (11 << 3)
#define LOG_LOCAL0   (16 << 3)
#define LOG_LOCAL1   (17 << 3)
#define LOG_LOCAL2   (18 << 3)
#define LOG_LOCAL3   (19 << 3)
#define LOG_LOCAL4   (20 << 3)
#define LOG_LOCAL5   (21 << 3)
#define LOG_LOCAL6   (22 << 3)
#define LOG_LOCAL7   (23 << 3)
#define LOG_FACMASK  0x03f8
#define LOG_FAC(p)   (((p) & LOG_FACMASK) >> 3)

#define LOG_MASK(pri) (1 << (pri))
#define LOG_UPTO(pri) ((1 << ((pri) + 1)) - 1)

/* openlog() options. */
#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

#ifdef SYSLOG_NAMES
/* The names logger(1) and syslog.conf use, as glibc and the BSDs give
 * them to a program that asks with SYSLOG_NAMES. */
#define INTERNAL_NOPRI 0x10
#define INTERNAL_MARK  (24 << 3)

typedef struct _code {
    const char *c_name;
    int         c_val;
} CODE;

static const CODE prioritynames[] = {
    { "alert", LOG_ALERT },     { "crit", LOG_CRIT },         { "debug", LOG_DEBUG },
    { "emerg", LOG_EMERG },     { "err", LOG_ERR },           { "error", LOG_ERR },
    { "info", LOG_INFO },       { "none", INTERNAL_NOPRI },   { "notice", LOG_NOTICE },
    { "panic", LOG_EMERG },     { "warn", LOG_WARNING },      { "warning", LOG_WARNING },
    { NULL, -1 },
};

static const CODE facilitynames[] = {
    { "auth", LOG_AUTH },       { "authpriv", LOG_AUTHPRIV }, { "cron", LOG_CRON },
    { "daemon", LOG_DAEMON },   { "ftp", LOG_FTP },           { "kern", LOG_KERN },
    { "lpr", LOG_LPR },         { "mail", LOG_MAIL },         { "mark", INTERNAL_MARK },
    { "news", LOG_NEWS },       { "security", LOG_AUTH },     { "syslog", LOG_SYSLOG },
    { "user", LOG_USER },       { "uucp", LOG_UUCP },         { "local0", LOG_LOCAL0 },
    { "local1", LOG_LOCAL1 },   { "local2", LOG_LOCAL2 },     { "local3", LOG_LOCAL3 },
    { "local4", LOG_LOCAL4 },   { "local5", LOG_LOCAL5 },     { "local6", LOG_LOCAL6 },
    { "local7", LOG_LOCAL7 },   { NULL, -1 },
};
#endif

void openlog(const char *, int, int);
void syslog(int, const char *, ...) __attribute__((__format__(__printf__, 2, 3)));
void vsyslog(int, const char *, va_list);
void closelog(void);
int  setlogmask(int);

_END_STD_C

#endif /* _SYSLOG_H_ */
