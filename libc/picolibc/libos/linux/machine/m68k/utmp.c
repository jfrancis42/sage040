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
 * utmp: who is logged in, as a file of fixed records -- Linux's
 * struct utmp (sys/utmp.h), in /var/run/utmp. login(1) writes a
 * USER_PROCESS record when a session starts and makes it DEAD_PROCESS
 * when it ends; who, w, uptime and top read it.
 *
 * The functions are glibc's: one file open at a time, a cursor through
 * it, a record returned in static storage. pututline() rewrites the
 * record for the same terminal (ut_id) in place, or appends; it creates
 * the file if it is missing, which glibc does not -- a machine with no
 * /var/run/utmp would otherwise never get one.
 */

#define _GNU_SOURCE
#include <sys/utmp.h>
#include <utmp.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static char ut_path[256] = _PATH_UTMP;
static int ut_fd = -1;
static int ut_writable;
static struct utmp ut_rec;

static int
ut_open(int write)
{
    if (ut_fd >= 0 && (!write || ut_writable))
        return 0;
    if (ut_fd >= 0)
        close(ut_fd);
    ut_writable = write;
    ut_fd = open(ut_path, write ? O_RDWR | O_CREAT | O_CLOEXEC
                                : O_RDONLY | O_CLOEXEC, 0644);
    return ut_fd < 0 ? -1 : 0;
}

int
utmpname(const char *file)
{
    size_t n = strlen(file);

    if (n >= sizeof(ut_path))
        return -1;
    endutent();
    memcpy(ut_path, file, n + 1);
    return 0;
}

void
setutent(void)
{
    if (ut_fd >= 0)
        lseek(ut_fd, 0, SEEK_SET);
}

void
endutent(void)
{
    if (ut_fd >= 0)
        close(ut_fd);
    ut_fd = -1;
}

struct utmp *
getutent(void)
{
    if (ut_open(0) < 0)
        return NULL;
    if (read(ut_fd, &ut_rec, sizeof(ut_rec)) != (ssize_t)sizeof(ut_rec))
        return NULL;            /* the end, or a torn last record */
    return &ut_rec;
}

/* Does `r` answer getutid's question `q`? */
static int
id_matches(const struct utmp *q, const struct utmp *r)
{
    switch (q->ut_type) {
    case RUN_LVL:
    case BOOT_TIME:
    case OLD_TIME:
    case NEW_TIME:
        return r->ut_type == q->ut_type;
    case INIT_PROCESS:
    case LOGIN_PROCESS:
    case USER_PROCESS:
    case DEAD_PROCESS:
        return (r->ut_type == INIT_PROCESS || r->ut_type == LOGIN_PROCESS ||
                r->ut_type == USER_PROCESS || r->ut_type == DEAD_PROCESS) &&
               memcmp(r->ut_id, q->ut_id, sizeof(r->ut_id)) == 0;
    }
    return 0;
}

struct utmp *
getutid(const struct utmp *q)
{
    struct utmp *r;

    while ((r = getutent()) != NULL)
        if (id_matches(q, r))
            return r;
    return NULL;
}

struct utmp *
getutline(const struct utmp *q)
{
    struct utmp *r;

    while ((r = getutent()) != NULL)
        if ((r->ut_type == USER_PROCESS || r->ut_type == LOGIN_PROCESS) &&
            strncmp(r->ut_line, q->ut_line, sizeof(r->ut_line)) == 0)
            return r;
    return NULL;
}

struct utmp *
pututline(const struct utmp *u)
{
    static struct utmp copy;
    off_t at = -1;

    copy = *u;                  /* u may be the static record itself */
    if (ut_open(1) < 0)
        return NULL;
    lseek(ut_fd, 0, SEEK_SET);
    for (;;) {
        off_t here = lseek(ut_fd, 0, SEEK_CUR);

        if (read(ut_fd, &ut_rec, sizeof(ut_rec)) != (ssize_t)sizeof(ut_rec))
            break;
        if (id_matches(&copy, &ut_rec)) {
            at = here;
            break;
        }
    }
    if (at < 0)
        at = lseek(ut_fd, 0, SEEK_END) / (off_t)sizeof(copy) * (off_t)sizeof(copy);
    if (lseek(ut_fd, at, SEEK_SET) != at ||
        write(ut_fd, &copy, sizeof(copy)) != (ssize_t)sizeof(copy))
        return NULL;
    ut_rec = copy;
    return &ut_rec;
}

void
updwtmp(const char *file, const struct utmp *u)
{
    int fd = open(file, O_WRONLY | O_APPEND | O_CLOEXEC);

    if (fd < 0)
        return;                 /* no wtmp kept: nothing to do */
    (void)write(fd, u, sizeof(*u));
    close(fd);
}
