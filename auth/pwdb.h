/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pwdb.h - /etc/passwd, /etc/shadow and /etc/group.
 *
 * Shared by login, su, sudo, passwd, useradd and userdel, because six
 * programs each with their own idea of the colon-separated format is
 * five chances for them to disagree about what an account is.
 *
 * Reading could have used picolibc's getpwnam(); writing could not --
 * there is no putpwent worth having and nothing at all for shadow --
 * and a reader and a writer that parse differently is how a file gets
 * corrupted. So both live here.
 */
#ifndef PWDB_H
#define PWDB_H

#include <sys/types.h>

#define PW_MAX_LINE   512
#define PW_MAX_NAME   64
#define PW_MAX_GROUPS 32

struct pwent {
    char name[PW_MAX_NAME];
    unsigned uid;
    unsigned gid;
    char gecos[128];
    char home[128];
    char shell[128];
};

/* Look an account up by name or by id. 0 if found. */
int pw_by_name(const char *name, struct pwent *out);
int pw_by_uid(unsigned uid, struct pwent *out);

/* The hash from /etc/shadow. Returns 0 and fills `hash` if there is an
 * entry; the hash may be "*" or "!", which no password can match. */
int sh_hash(const char *name, char *hash, unsigned len);

/* Replace one account's hash, leaving the rest of the file alone. */
int sh_set_hash(const char *name, const char *hash);

/* Every group `name` is in, primary first. Returns how many. */
int gr_of_user(const char *name, unsigned primary, gid_t *gids, int max);

/* The gid of a named group, or -1. */
long gr_gid(const char *group);

/* Add and remove accounts. pw_add writes /etc/passwd, /etc/shadow and
 * /etc/group; pw_del takes all three back out. */
int pw_add(const struct pwent *p, const char *hash);
int pw_del(const char *name);

/* The next free uid at or above `from`. */
unsigned pw_next_uid(unsigned from);

/* Read a password without echoing it. Returns 0, or -1 if there is no
 * terminal to ask on. */
int read_password(const char *prompt, char *buf, unsigned len);

/* A fresh "$6$" salt, from /dev/urandom. */
void make_salt(char *out, unsigned len);

#endif /* PWDB_H */
