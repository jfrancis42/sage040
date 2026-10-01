/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * events.h - eventfd, timerfd, signalfd, epoll and inotify. See events.c.
 */
#ifndef EVENTS_H
#define EVENTS_H

#include "kernel.h"
#include "uapi.h"

struct file;
struct pt_regs;

/* The system calls, from syslinux.c. Each returns what the call does. */
s32 sys_eventfd2(u32 initval, int flags);
s32 sys_timerfd_create(int clockid, int flags);
s32 sys_timerfd_settime(int fd, int flags, u32 unew, u32 uold, int wide);

/* POSIX timers: timer_create and the rest, timerfds that signal. */
s32  sys_timer_create(int clockid, u32 usev, u32 uid);
s32  sys_timer_settime(u32 id, int flags, u32 unew, u32 uold, int wide);
s32  sys_timer_gettime(u32 id, u32 ucur, int wide);
s32  sys_timer_getoverrun(u32 id);
s32  sys_timer_delete(u32 id);
void posix_timer_tick(void);            /* task_timeouts, every tick */
void posix_timer_exit(int tgid);        /* exec and exit             */
s32 sys_timerfd_gettime(int fd, u32 ucur, int wide);
s32 sys_signalfd4(int fd, u32 umask, u32 size, int flags);
s32 sys_epoll_create1(int flags);
s32 sys_epoll_ctl(int epfd, int op, int fd, u32 uevent);
s32 sys_epoll_pwait(int epfd, u32 uevents, int max, s32 timeout_ms,
                    u32 umask, u32 masksize);
s32 sys_inotify_init1(int flags);
s32 sys_inotify_add_watch(int fd, u32 upath, u32 mask);
s32 sys_inotify_rm_watch(int fd, int wd);

/*
 * Is this one of events.c's files? Their reads hand over whole records
 * (8 bytes, 128, an inotify event), which must not be split at a page of
 * the caller's buffer the way rw_user splits a read -- so syscall.c
 * reads them through a kernel buffer instead.
 */
int events_owns(struct file *f);

/* A file is being freed: no epoll may go on watching it. From file_put. */
void events_file_gone(struct file *f);

/*
 * INOTIFY'S HOOKS, called by vfs.c after a change succeeds. They cost a
 * test of one word unless something is being watched.
 *
 * inotify_path(): `path` (as the caller gave it) had `self` happen to it,
 * and its directory sees `parent` about the name. inotify_file(): the
 * same, for an open file. Removal and renaming have to look at the
 * victim BEFORE it goes, into a struct inotify_victim, and report after.
 */
extern int inotify_watching;

void inotify_path(const char *path, u32 parent, u32 self);
void inotify_file(struct file *f, u32 mask);

struct inotify_victim {
    int valid;
    u32 ino;
    u32 nlink;
    int isdir;
};

void inotify_look(const char *path, struct inotify_victim *v);
void inotify_removed(const char *path, const struct inotify_victim *v);
void inotify_moved(const char *from, const char *to,
                   const struct inotify_victim *v,
                   const struct inotify_victim *replaced);

#endif /* EVENTS_H */
