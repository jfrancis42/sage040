/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 Jeff Francis */
/*
 * shadow.h - the password hashes, kept out of /etc/passwd.
 *
 * /etc/passwd has to be world-readable: every getpwuid() reads it to
 * turn a uid into a name. A hash in a world-readable file is a hash
 * anybody can attack offline, so the hash lives in /etc/shadow, which
 * is 0600 root, and the passwd entry carries an 'x' instead.
 *
 * A program that needs the hash -- login, su, sudo, passwd, sshd --
 * asks for it HERE, and gets NULL with EACCES if it is not root. That
 * is the whole point: the split only means something because the
 * kernel enforces the mode (vfs_may in kernel/vfs.c).
 *
 * The layout is Linux's, field for field, because dropbear and
 * everything else portable expects exactly this structure.
 */
#ifndef _SHADOW_H_
#define _SHADOW_H_

#include <sys/cdefs.h>
#include <stdio.h>

#define SHADOW "/etc/shadow"

struct spwd {
    char          *sp_namp;     /* login name */
    char          *sp_pwdp;     /* the hash, or "*" / "!" for no login */
    long int       sp_lstchg;   /* days since 1970 of the last change */
    long int       sp_min;      /* days before it may be changed again */
    long int       sp_max;      /* days after which it must be */
    long int       sp_warn;     /* days of warning before that */
    long int       sp_inact;    /* days after expiry before disabling */
    long int       sp_expire;   /* days since 1970 when it expires */
    unsigned long int sp_flag;  /* reserved */
};

_BEGIN_STD_C

struct spwd *getspnam(const char *__name);
struct spwd *getspent(void);
void         setspent(void);
void         endspent(void);
int          getspnam_r(const char *__name, struct spwd *__result,
                        char *__buf, size_t __buflen,
                        struct spwd **__resultp);

_END_STD_C

#endif /* _SHADOW_H_ */
