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
 * sys/personality.h -- picolibc 1.8.12 has none. Linux's execution
 * domains and flags. The kernel has one domain, PER_LINUX, and keeps
 * whatever flags it is given; ADDR_NO_RANDOMIZE is true whether set or
 * not, since nothing is randomized.
 */
#ifndef _SYS_PERSONALITY_H_
#define _SYS_PERSONALITY_H_

#include <sys/cdefs.h>

_BEGIN_STD_C

enum {
    UNAME26            = 0x0020000,
    ADDR_NO_RANDOMIZE  = 0x0040000,
    FDPIC_FUNCPTRS     = 0x0080000,
    MMAP_PAGE_ZERO     = 0x0100000,
    ADDR_COMPAT_LAYOUT = 0x0200000,
    READ_IMPLIES_EXEC  = 0x0400000,
    ADDR_LIMIT_32BIT   = 0x0800000,
    SHORT_INODE        = 0x1000000,
    WHOLE_SECONDS      = 0x2000000,
    STICKY_TIMEOUTS    = 0x4000000,
    ADDR_LIMIT_3GB     = 0x8000000
};

#define PER_MASK 0x00ff
enum { PER_LINUX = 0x0000, PER_LINUX32 = 0x0008 };

int personality(unsigned long);

_END_STD_C

#endif /* _SYS_PERSONALITY_H_ */
