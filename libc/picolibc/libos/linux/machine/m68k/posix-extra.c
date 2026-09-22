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
 * pause, usleep and select are not in picolibc's libos/linux (1.8.12)
 * on any architecture. They are here, in the m68k backend, only so that
 * the release underneath stays unmodified; nothing in them is specific
 * to m68k, and they belong beside the other calls in libos/linux.
 */

#include "../../local-linux.h"
#include <sys/select.h>
#include <time.h>

int
pause(void)
{
    return syscall(LINUX_SYS_pause);
}

int
usleep(useconds_t usec)
{
    struct timespec ts;

    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (long)(usec % 1000000) * 1000;
    return nanosleep(&ts, NULL);
}

/*
 * Through _newselect, whose timeout is Linux/m68k's 32-bit timeval.
 * picolibc's time_t is 64 bits, so it is converted each way. The fd_set
 * needs nothing: both sides keep bit n in word n / 32.
 */
int
select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct {
        __int32_t tv_sec;
        __int32_t tv_usec;
    } ktv, *ktvp = NULL;
    int ret;

    if (tv) {
        ktv.tv_sec = (__int32_t)tv->tv_sec;
        ktv.tv_usec = (__int32_t)tv->tv_usec;
        ktvp = &ktv;
    }
    ret = syscall(LINUX_SYS__newselect, n, rd, wr, ex, ktvp);
    if (tv) {
        /* Linux writes back the time left, and so does this. */
        tv->tv_sec = ktv.tv_sec;
        tv->tv_usec = ktv.tv_usec;
    }
    return ret;
}
