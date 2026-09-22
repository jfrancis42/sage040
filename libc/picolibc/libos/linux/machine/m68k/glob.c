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

#define _GNU_SOURCE             /* every declaration picolibc has */

/*
 * glob() and globfree(), which picolibc 1.8.12 declares and does not
 * define. POSIX's: the pattern is matched a component at a time against
 * directory entries with fnmatch(); a leading '.' is matched only by a
 * pattern that starts with one; components with no pattern characters
 * are taken as they are and must exist. GLOB_ERR, GLOB_MARK,
 * GLOB_NOCHECK, GLOB_NOESCAPE, GLOB_NOSORT, GLOB_DOOFFS and GLOB_APPEND
 * are honoured; the BSD extensions picolibc's header also names
 * (GLOB_BRACE, GLOB_TILDE, GLOB_ALTDIRFUNC...) are not implemented and
 * are ignored.
 */

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef GLOB_NOESCAPE
#define GLOB_NOESCAPE 0x2000
#endif

struct found {
    char **v;
    size_t n, cap;
};

static int
add(struct found *f, const char *s)
{
    char *c;

    if (f->n == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 16;
        char **v = realloc(f->v, cap * sizeof(*v));

        if (!v)
            return GLOB_NOSPACE;
        f->v = v;
        f->cap = cap;
    }
    c = malloc(strlen(s) + 1);
    if (!c)
        return GLOB_NOSPACE;
    strcpy(c, s);
    f->v[f->n++] = c;
    return 0;
}

static int
has_magic(const char *s, size_t len, int noescape)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (s[i] == '*' || s[i] == '?' || s[i] == '[')
            return 1;
        if (s[i] == '\\' && !noescape && i + 1 < len)
            i++;
    }
    return 0;
}

/* A component without magic, its backslashes removed. */
static void
unescape(char *out, const char *s, size_t len, int noescape)
{
    size_t i, n = 0;

    for (i = 0; i < len; i++) {
        if (s[i] == '\\' && !noescape && i + 1 < len)
            i++;
        out[n++] = s[i];
    }
    out[n] = '\0';
}

struct ctx {
    int          flags;
    int          (*errfunc)(const char *, int);
    struct found found;
};

/*
 * Match `pat` (what is left of the pattern) below `path` (what has been
 * matched so far, "" at the start).
 */
static int
expand(struct ctx *c, char *path, size_t plen, const char *pat)
{
    const char *end;
    size_t      clen;
    int         noescape = (c->flags & GLOB_NOESCAPE) != 0;
    int         last, err = 0;

    while (*pat == '/') {           /* runs of slashes are one */
        if (plen + 1 >= PATH_MAX)
            return 0;
        path[plen++] = '/';
        path[plen] = '\0';
        pat++;
    }
    if (!*pat) {
        struct stat st;

        if (plen == 0 || lstat(path, &st) < 0)
            return 0;
        /* GLOB_MARK: a directory gets a slash, unless it has one. */
        if ((c->flags & GLOB_MARK) && S_ISDIR(st.st_mode) && path[plen - 1] != '/' &&
            plen + 1 < PATH_MAX) {
            path[plen] = '/';
            path[plen + 1] = '\0';
        }
        err = add(&c->found, path);
        path[plen] = '\0';
        return err;
    }

    end = strchr(pat, '/');
    clen = end ? (size_t)(end - pat) : strlen(pat);
    last = !end;

    if (!has_magic(pat, clen, noescape)) {
        if (plen + clen + 1 >= PATH_MAX)
            return 0;
        unescape(path + plen, pat, clen, noescape);
        err = expand(c, path, plen + strlen(path + plen), pat + clen);
        path[plen] = '\0';
        return err;
    }

    {
        char           comp[NAME_MAX * 2 + 2];
        DIR           *d;
        struct dirent *de;

        if (clen >= sizeof(comp))
            return 0;
        memcpy(comp, pat, clen);
        comp[clen] = '\0';

        d = opendir(plen ? path : ".");
        if (!d) {
            if ((c->errfunc && c->errfunc(plen ? path : ".", errno)) || (c->flags & GLOB_ERR))
                return GLOB_ABEND;
            return 0;
        }
        while ((de = readdir(d)) != NULL) {
            size_t nl = strlen(de->d_name);

            if (de->d_name[0] == '.' && comp[0] != '.')
                continue;       /* hidden unless asked for by name */
            if (fnmatch(comp, de->d_name, FNM_PERIOD | (noescape ? FNM_NOESCAPE : 0)) != 0)
                continue;
            if (plen + nl + 1 >= PATH_MAX)
                continue;
            memcpy(path + plen, de->d_name, nl + 1);
            if (last) {
                err = expand(c, path, plen + nl, "");
            } else {
                struct stat st;

                /* Only a directory can hold what the rest names. */
                if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                    err = expand(c, path, plen + nl, pat + clen);
            }
            path[plen] = '\0';
            if (err)
                break;
        }
        closedir(d);
    }
    return err;
}

static int
compare(const void *a, const void *b)
{
    return strcoll(*(char *const *)a, *(char *const *)b);
}

int
glob(const char *__restrict pattern, int flags, int (*errfunc)(const char *, int),
     glob_t *__restrict g)
{
    struct ctx c;
    char       path[PATH_MAX];
    size_t     offs, old, i;
    int        err;
    char     **v;

    if (!(flags & GLOB_APPEND)) {
        g->gl_pathc = 0;
        g->gl_pathv = NULL;
        if (!(flags & GLOB_DOOFFS))
            g->gl_offs = 0;
    }
    offs = (flags & GLOB_DOOFFS) ? (size_t)g->gl_offs : 0;
    old = (size_t)g->gl_pathc;

    memset(&c, 0, sizeof(c));
    c.flags = flags;
    c.errfunc = errfunc;
    path[0] = '\0';
    err = expand(&c, path, 0, pattern);

    if (err == 0 && c.found.n == 0) {
        if (flags & GLOB_NOCHECK) {
            if (add(&c.found, pattern))
                err = GLOB_NOSPACE;
        } else {
            err = GLOB_NOMATCH;
        }
    }
    if (err == 0 && !(flags & GLOB_NOSORT) && c.found.n > 1)
        qsort(c.found.v, c.found.n, sizeof(char *), compare);

    if (c.found.n) {
        v = realloc(g->gl_pathv, (offs + old + c.found.n + 1) * sizeof(char *));
        if (!v) {
            err = GLOB_NOSPACE;
        } else {
            if (!old)
                for (i = 0; i < offs; i++)
                    v[i] = NULL;
            for (i = 0; i < c.found.n; i++)
                v[offs + old + i] = c.found.v[i];
            v[offs + old + c.found.n] = NULL;
            g->gl_pathv = v;
            g->gl_pathc = (int)(old + c.found.n);
            c.found.n = 0;
        }
    }
    for (i = 0; i < c.found.n; i++)
        free(c.found.v[i]);
    free(c.found.v);
    g->gl_flags = flags;
    return err;
}

void
globfree(glob_t *g)
{
    size_t offs = (g->gl_flags & GLOB_DOOFFS) ? (size_t)g->gl_offs : 0;
    int    i;

    if (g->gl_pathv) {
        for (i = 0; i < g->gl_pathc; i++)
            free(g->gl_pathv[offs + i]);
        free(g->gl_pathv);
    }
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}
