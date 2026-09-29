/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * procfs.h - /proc, synthesised from the kernel's own tables.
 *
 * See procfs.c. vfs.c is the only caller: every path-taking operation
 * asks proc_owns() first, and a path under /proc is resolved here --
 * its symbolic links followed -- before anything else looks at it.
 */
#ifndef PROCFS_H
#define PROCFS_H

#include "kernel.h"
#include "uapi.h"

struct file;

/* Is this "/proc" or a path under it -- absolute, or relative from a
 * working directory in /proc? */
int proc_owns(const char *path);

/* Is the caller's working directory in /proc? */
int proc_cwd(void);

/*
 * Follow /proc's symbolic links -- self, and a process's exe, cwd, root
 * and fd/N -- through `path`, the last component too when `follow` is
 * set. The answer is one of:
 *
 *   PROC_HERE       `out` is a path inside /proc, with no link left in
 *                   it but perhaps the last: procfs answers it
 *   PROC_ELSEWHERE  `out` is a path outside /proc, which the caller
 *                   carries on with as though it had been given it
 *   PROC_ANON       the last link leads to an open file that has no
 *                   path (a pipe, a socket); *anon holds a reference
 *
 * or -errno: ENOENT for a name /proc does not have, EACCES for another
 * user's process's links, ENOTDIR for a path that goes on through a
 * pipe. `out` is PATH_MAX bytes.
 */
#define PROC_HERE       1
#define PROC_ELSEWHERE  2
#define PROC_ANON       3
int proc_lookup(const char *path, int follow, char *out, struct file **anon);

/* Of a PROC_HERE path. */
int proc_stat(const char *path, struct stat *st);
int proc_readlink(const char *path, char *out, u32 size);
int proc_open(const char *path, int flags);     /* a descriptor, or -errno */

/* An open directory in /proc: what getdents64 and the *at calls need. */
int proc_is_dir_file(struct file *f);
int proc_dir_path(struct file *f, char *out, u32 size);
s32 proc_getdents64(struct file *f, u8 *buf, u32 len);

#endif /* PROCFS_H */
