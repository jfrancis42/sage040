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
 * <stdio_ext.h>: what state a FILE is in. picolibc's buffered streams
 * (fopen, and stdin/stdout/stderr here) are a struct __file_bufio, whose
 * `dir` says whether the last operation read or wrote; in the read
 * direction buf[off..len) is data not yet consumed, in the write
 * direction buf[0..len) is data not yet written. Any other stream -- a
 * string stream -- has no buffer to ask about, and answers from its
 * flags alone.
 */

#include <stdio.h>
#include <stdio-bufio.h>
#include <stdio_ext.h>

static struct __file_bufio *
bufio_of(FILE *fp)
{
    return (fp->flags & __SBUF) ? (struct __file_bufio *)fp : NULL;
}

size_t
__fbufsize(FILE *fp)
{
    struct __file_bufio *bf = bufio_of(fp);

    return bf ? (size_t)bf->size : 0;
}

int
__freadable(FILE *fp)
{
    return (fp->flags & __SRD) != 0;
}

int
__fwritable(FILE *fp)
{
    return (fp->flags & __SWR) != 0;
}

/* Read-only, or open for both and the last thing done was a read. */
int
__freading(FILE *fp)
{
    struct __file_bufio *bf = bufio_of(fp);

    if (!(fp->flags & __SRD))
        return 0;
    if (!(fp->flags & __SWR))
        return 1;
    return bf && bf->dir == __SRD;
}

int
__fwriting(FILE *fp)
{
    struct __file_bufio *bf = bufio_of(fp);

    if (!(fp->flags & __SWR))
        return 0;
    if (!(fp->flags & __SRD))
        return 1;
    return bf && bf->dir == __SWR;
}

int
__flbf(FILE *fp)
{
    struct __file_bufio *bf = bufio_of(fp);

    return bf && (bf->bflags & __BLBF);
}

size_t
__fpending(FILE *fp)
{
    struct __file_bufio *bf = bufio_of(fp);

    return (bf && bf->dir == __SWR) ? (size_t)bf->len : 0;
}

size_t
__freadahead(FILE *fp)
{
    struct __file_bufio *bf = bufio_of(fp);
    size_t n = fp->unget ? 1 : 0;

    if (bf && bf->dir == __SRD && bf->len > bf->off)
        n += (size_t)(bf->len - bf->off);
    return n;
}

/* Discard whatever is buffered, either way, and anything pushed back. */
void
__fpurge(FILE *fp)
{
    struct __file_bufio *bf = bufio_of(fp);

    fp->unget = 0;
    if (!bf)
        return;
    if (bf->dir == __SRD)
        bf->off = bf->len;
    else
        bf->len = 0;
}

/* One thread per process: the caller never needs to lock. */
int
__fsetlocking(FILE *fp, int type)
{
    (void)fp;
    (void)type;
    return FSETLOCKING_INTERNAL;
}

void
_flushlbf(void)
{
    fflush(NULL);
}

/* __fseterr is picolibc's own (libc/stdio/fseterr.c). */
