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
 * inet.c - addresses as text, IPv4.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

/* inet_aton's classic grammar: a, a.b, a.b.c or a.b.c.d, each part
 * decimal, octal (0...) or hex (0x...). */
int inet_aton(const char *cp, struct in_addr *inp)
{
    uint32_t parts[4], val;
    int n = 0;

    for (;;) {
        int base = 10, digits = 0;

        val = 0;
        if (*cp == '0') {
            base = 8;
            if (cp[1] == 'x' || cp[1] == 'X') {
                base = 16;
                cp += 2;
            }
        }
        for (;; cp++) {
            int d;

            if (*cp >= '0' && *cp <= '9') {
                d = *cp - '0';
            } else if (base == 16 && *cp >= 'a' && *cp <= 'f') {
                d = *cp - 'a' + 10;
            } else if (base == 16 && *cp >= 'A' && *cp <= 'F') {
                d = *cp - 'A' + 10;
            } else {
                break;
            }
            if (d >= base || val > (0xffffffffu - (uint32_t)d) / (uint32_t)base) {
                return 0;
            }
            val = val * (uint32_t)base + (uint32_t)d;
            digits++;
        }
        if (!digits && base != 8) {
            return 0;
        }
        if (n == 4) {
            return 0;
        }
        parts[n++] = val;
        if (*cp == '.') {
            cp++;
            continue;
        }
        break;
    }
    if (*cp != '\0') {
        return 0;
    }
    switch (n) {
    case 1:
        val = parts[0];
        break;
    case 2:
        if (parts[0] > 0xff || parts[1] > 0xffffff) return 0;
        val = parts[0] << 24 | parts[1];
        break;
    case 3:
        if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xffff) return 0;
        val = parts[0] << 24 | parts[1] << 16 | parts[2];
        break;
    default:
        if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xff ||
            parts[3] > 0xff) return 0;
        val = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
        break;
    }
    if (inp) {
        inp->s_addr = htonl(val);
    }
    return 1;
}

in_addr_t inet_addr(const char *cp)
{
    struct in_addr a;

    return inet_aton(cp, &a) ? a.s_addr : INADDR_NONE;
}

const char *inet_ntop(int af, const void *src, char *dst, unsigned int size)
{
    const unsigned char *p = src;
    char buf[INET_ADDRSTRLEN];

    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return 0;
    }
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    if (strlen(buf) + 1 > size) {
        errno = ENOSPC;
        return 0;
    }
    strcpy(dst, buf);
    return dst;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[INET_ADDRSTRLEN];

    return (char *)inet_ntop(AF_INET, &in, buf, sizeof(buf));
}

/* Strictly four decimal parts, as POSIX has inet_pton read them -- not
 * inet_aton's looser grammar. */
int inet_pton(int af, const char *src, void *dst)
{
    unsigned char out[4];
    int n = 0;

    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    while (n < 4) {
        unsigned v = 0;
        int digits = 0;

        while (*src >= '0' && *src <= '9') {
            if (digits && v == 0) {
                return 0;               /* no leading zeroes */
            }
            v = v * 10 + (unsigned)(*src++ - '0');
            if (++digits > 3 || v > 255) {
                return 0;
            }
        }
        if (!digits) {
            return 0;
        }
        out[n++] = (unsigned char)v;
        if (n < 4 && *src++ != '.') {
            return 0;
        }
    }
    if (*src != '\0') {
        return 0;
    }
    memcpy(dst, out, 4);
    return 1;
}
