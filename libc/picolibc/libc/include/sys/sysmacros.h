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
 * sys/sysmacros.h -- picolibc 1.8.12 has none. major(), minor() and
 * makedev() in picolibc's own encoding of a dev_t -- the one its stat()
 * fills in and <sys/stat.h> already decodes under __BSD_VISIBLE: the
 * major number in the upper half, the minor in the lower. (Converting
 * to the kernel's encoding is the system call wrappers' business.)
 */
#ifndef _SYS_SYSMACROS_H_
#define _SYS_SYSMACROS_H_

#include <sys/types.h>

#ifndef _major_dev_shift
#define _major_dev_shift ((sizeof(dev_t) >> 1) << 3)
#endif
#ifndef major
#define major(d) ((d) >> _major_dev_shift)
#endif
#ifndef minor
#define minor(d) ((d) & (((dev_t)1 << _major_dev_shift) - 1))
#endif
#define makedev(ma, mi) ((dev_t)(ma) << _major_dev_shift | (dev_t)(mi))

#endif /* _SYS_SYSMACROS_H_ */
