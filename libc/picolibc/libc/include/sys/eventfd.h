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

/*
 * sys/eventfd.h -- picolibc 1.8.12 has none. Linux's, with Linux's flag
 * values (which are O_CLOEXEC and O_NONBLOCK in LINUX's numbering, not
 * picolibc's: passed to the kernel as they are). The calls are in
 * libos/linux/machine/m68k/events.c.
 */
#ifndef _SYS_EVENTFD_H_
#define _SYS_EVENTFD_H_

#include <sys/cdefs.h>
#include <stdint.h>

_BEGIN_STD_C

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE   0x00001
#define EFD_NONBLOCK    0x00800
#define EFD_CLOEXEC     0x80000

int eventfd(unsigned int __initval, int __flags);
int eventfd_read(int __fd, eventfd_t *__value);
int eventfd_write(int __fd, eventfd_t __value);

_END_STD_C

#endif /* _SYS_EVENTFD_H_ */
