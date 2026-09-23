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
 * arpa/inet.h - byte order, and addresses as text.
 *
 * Replaces picolibc's, which has the byte-order macros and nothing
 * else; those are reproduced here as picolibc defines them.
 */
#ifndef __ARPA_INET_H__
#define __ARPA_INET_H__


#include <sys/cdefs.h>

/*
 * EVERYTHING BELOW IS C, even when a C++ program includes it.
 *
 * Without this, a C++ translation unit gives every one of these
 * functions a C++ mangled name, and the link fails on symbols like
 * `_Z11getaddrinfoPKcS0_PK8addrinfoPPS1_` -- a name no C library
 * contains. It stayed unnoticed for as long as nothing here was
 * written in C++; gcc's own c++tools is, and found it at once.
 */

_BEGIN_STD_C

#include <endian.h>
#include <stdint.h>

#ifndef __machine_host_to_from_network_defined
#if _BYTE_ORDER == _LITTLE_ENDIAN
#define __htonl(_x) __bswap32(_x)
#define __htons(_x) __bswap16(_x)
#define __ntohl(_x) __bswap32(_x)
#define __ntohs(_x) __bswap16(_x)
#define htonl(_x)   __htonl(_x)
#define htons(_x)   __htons(_x)
#define ntohl(_x)   __htonl(_x)
#define ntohs(_x)   __htons(_x)
#else
#define __htonl(_x) ((__uint32_t)(_x))
#define __htons(_x) ((__uint16_t)(_x))
#define __ntohl(_x) ((__uint32_t)(_x))
#define __ntohs(_x) ((__uint16_t)(_x))
#define htonl(_x)   __htonl(_x)
#define htons(_x)   __htons(_x)
#define ntohl(_x)   __ntohl(_x)
#define ntohs(_x)   __ntohs(_x)
#endif
#endif /* __machine_host_to_from_network_defined */

struct in_addr;

uint32_t    inet_addr(const char *cp);
int         inet_aton(const char *cp, struct in_addr *inp);
char       *inet_ntoa(struct in_addr in);
int         inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, unsigned int size);

_END_STD_C

#endif /* __ARPA_INET_H__ */
