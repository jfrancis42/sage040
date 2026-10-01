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
 * if.c -- network interfaces by name and number.
 *
 * The kernel lists its interfaces through netctl(NETCTL_INFO), by a
 * number counting from 0, and says EINVAL past the last. An interface's
 * INDEX is that number plus one, as Linux's indexes start at 1 -- so lo
 * and eth0 are whatever the kernel lists first and second, and the
 * answers here are the kernel's, not a table.
 */
#include <errno.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NR_netctl     1002
#define NETCTL_INFO   0

long syscall(long nr, ...);

/*
 * The kernel's struct netinfo is longer than the name, and grows: the
 * answer lands in room enough for any of it (see resolv.c on what a
 * copy of exactly the struct cost once). Only the name, first, is read.
 */
#define NETINFO_ROOM  256

static int name_of(unsigned int i, char *name)
{
    union {
        char name[8];
        unsigned char room[NETINFO_ROOM];
    } u;

    memset(&u, 0, sizeof(u));
    if (syscall(NR_netctl, NETCTL_INFO, (long)i, &u) != 0) {
        return -1;
    }
    memcpy(name, u.name, 8);
    name[8] = '\0';
    return 0;
}

char *if_indextoname(unsigned int ifindex, char *ifname)
{
    char name[IF_NAMESIZE];

    if (ifindex == 0 || name_of(ifindex - 1, name) < 0) {
        errno = ENXIO;
        return NULL;
    }
    strcpy(ifname, name);
    return ifname;
}

unsigned int if_nametoindex(const char *ifname)
{
    char name[IF_NAMESIZE];
    unsigned int i;

    for (i = 0; name_of(i, name) == 0; i++) {
        if (strcmp(name, ifname) == 0) {
            return i + 1;
        }
    }
    errno = ENODEV;
    return 0;
}

struct if_nameindex *if_nameindex(void)
{
    struct if_nameindex *v;
    char name[IF_NAMESIZE];
    unsigned int n = 0, i;

    while (name_of(n, name) == 0) {
        n++;
    }
    v = calloc(n + 1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (name_of(i, name) < 0 || !(v[i].if_name = strdup(name))) {
            if_freenameindex(v);
            errno = ENOBUFS;
            return NULL;
        }
        v[i].if_index = i + 1;
    }
    return v;
}

void if_freenameindex(struct if_nameindex *ptr)
{
    struct if_nameindex *p;

    if (!ptr) {
        return;
    }
    for (p = ptr; p->if_name; p++) {
        free(p->if_name);
    }
    free(ptr);
}
