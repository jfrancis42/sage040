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

#define _GNU_SOURCE

/*
 * sendfile, splice, copy_file_range, mremap and memfd_create: none is in
 * picolibc's libos/linux (1.8.12). off_t is 64 bits in this library,
 * which is exactly Linux's loff_t, so the offsets pass straight through
 * -- sendfile by way of sendfile64, whose offset is the loff_t one.
 */

#include "../../local-linux.h"
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <unistd.h>

ssize_t
sendfile(int out, int in, off_t *offset, size_t count)
{
    return syscall(LINUX_SYS_sendfile64, out, in, offset, count);
}

ssize_t
splice(int in, off_t *off_in, int out, off_t *off_out, size_t len,
       unsigned int flags)
{
    return syscall(LINUX_SYS_splice, in, off_in, out, off_out, len, flags);
}

ssize_t
copy_file_range(int in, off_t *off_in, int out, off_t *off_out, size_t len,
                unsigned int flags)
{
    return syscall(LINUX_SYS_copy_file_range, in, off_in, out, off_out, len,
                   flags);
}

/* The fifth argument, the new address, exists only with MREMAP_FIXED. */
void *
mremap(void *old, size_t old_len, size_t new_len, int flags, ...)
{
    void *new_addr = 0;
    long r;

    if (flags & MREMAP_FIXED) {
        va_list ap;

        va_start(ap, flags);
        new_addr = va_arg(ap, void *);
        va_end(ap);
    }
    r = syscall(LINUX_SYS_mremap, old, old_len, new_len, flags, new_addr);
    return r == -1 ? MAP_FAILED : (void *)r;
}

int
memfd_create(const char *name, unsigned int flags)
{
    return syscall(LINUX_SYS_memfd_create, name, flags);
}
