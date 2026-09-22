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
 * semaphore.h - POSIX semaphores, over the same futex the mutexes use.
 *
 * Unnamed only: sem_open and friends would want a filesystem that can
 * hold a name for something that is not a file, and FAT cannot. They
 * are not declared here rather than declared and failing.
 */
#ifndef _SEMAPHORE_H_
#define _SEMAPHORE_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

_BEGIN_STD_C

typedef struct {
    volatile int count;
    volatile int waiters;
} sem_t;

#define SEM_VALUE_MAX   2147483647

int sem_init(sem_t *__sem, int __pshared, unsigned __value);
int sem_destroy(sem_t *__sem);
int sem_wait(sem_t *__sem);
int sem_trywait(sem_t *__sem);
int sem_timedwait(sem_t *__sem, const struct timespec *__abstime);
int sem_post(sem_t *__sem);
int sem_getvalue(sem_t *__sem, int *__value);

_END_STD_C

#endif /* _SEMAPHORE_H_ */
