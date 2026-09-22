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
 * syslog(), for a machine with no syslog daemon.
 *
 * Each message is one line -- "Mon DD HH:MM:SS host ident[pid]: text" --
 * appended to /var/log/syslog when that directory exists, which is
 * where a daemon would have put it. If it cannot be written there, it
 * goes to /dev/console only when openlog() asked for LOG_CONS, as POSIX
 * has it; LOG_PERROR also copies it to stderr. %m is the text of errno.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static const char *log_ident;
static int         log_opts;
static int         log_facility = LOG_USER;
static int         log_mask = 0xff;

void
openlog(const char *ident, int option, int facility)
{
    log_ident = ident;
    log_opts = option;
    if (facility & ~LOG_FACMASK)
        facility = LOG_USER;
    log_facility = facility ? facility : LOG_USER;
}

void
closelog(void)
{
    log_ident = NULL;
    log_opts = 0;
    log_facility = LOG_USER;
}

int
setlogmask(int mask)
{
    int old = log_mask;

    if (mask)
        log_mask = mask;
    return old;
}

void
vsyslog(int pri, const char *fmt, va_list ap)
{
    char       line[1024], fmt2[512];
    char       host[65];
    const char *f;
    size_t     n = 0;
    int        saved = errno, fd, i;
    time_t     now = time(NULL);
    struct tm  tm;

    if (!(log_mask & LOG_MASK(LOG_PRI(pri))))
        return;

    /* %m first: the text of errno, as it was on the way in. */
    for (f = fmt, i = 0; *f && i < (int)sizeof(fmt2) - 1; f++) {
        if (f[0] == '%' && f[1] == 'm') {
            const char *e = strerror(saved);

            for (; *e && i < (int)sizeof(fmt2) - 1; e++)
                fmt2[i++] = (*e == '%') ? ' ' : *e;     /* no % into the format */
            f++;
        } else {
            fmt2[i++] = *f;
        }
    }
    fmt2[i] = '\0';

    localtime_r(&now, &tm);
    n += strftime(line, sizeof(line), "%b %e %H:%M:%S ", &tm);
    if (gethostname(host, sizeof(host)) < 0)
        strcpy(host, "localhost");
    n += (size_t)snprintf(line + n, sizeof(line) - n, "%s %s", host,
                          log_ident ? log_ident : (getprogname() ? getprogname() : "?"));
    if (n < sizeof(line) && (log_opts & LOG_PID))
        n += (size_t)snprintf(line + n, sizeof(line) - n, "[%d]", (int)getpid());
    if (n < sizeof(line))
        n += (size_t)snprintf(line + n, sizeof(line) - n, ": ");
    if (n < sizeof(line))
        n += (size_t)vsnprintf(line + n, sizeof(line) - n, fmt2, ap);
    if (n >= sizeof(line) - 1)
        n = sizeof(line) - 2;
    if (n == 0 || line[n - 1] != '\n')
        line[n++] = '\n';

    /*
     * The same file klogd writes the kernel's messages to, so that a
     * machine has ONE log rather than one per source. The kernel's
     * lines say "kernel:", these say the program's name, and `cat
     * /var/log/syslog` is the whole story in the order it happened.
     */
    fd = open("/var/log/syslog", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) {
        write(fd, line, n);
        close(fd);
    } else if (log_opts & LOG_CONS) {
        fd = open("/dev/console", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            write(fd, line, n);
            close(fd);
        }
    }
    if (log_opts & LOG_PERROR)
        write(2, line, n);
    errno = saved;
}

void
syslog(int pri, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsyslog(pri, fmt, ap);
    va_end(ap);
}
