/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * xfer.c - sendfile, splice and copy_file_range: bytes from one
 * descriptor to another without passing through the program.
 *
 * On Linux these avoid copies; here they save the program a buffer and
 * two system calls a chunk, and that is all -- one page of the kernel's
 * memory carries the bytes, a read and a write at a time. What matters
 * to a port is that the calls exist with Linux's rules, because several
 * (a web server's sendfile, cp's copy_file_range, a proxy's splice)
 * have no fallback, or one that is only taken on ENOSYS.
 *
 * Each call moves what it can and reports how much. A reader that has
 * given something is not waited on for more: after the first chunk a
 * pipe or a socket is only read while it has bytes waiting, as read()
 * itself behaves (see rw_user).
 */
#include "kernel.h"
#include "uapi.h"
#include "vfs.h"
#include "dev.h"
#include "pipe.h"
#include "poll.h"
#include "pmm.h"
#include "sysint.h"
#include "errno.h"
#include "string.h"

#define XFER_MAX    0x7ffff000UL    /* Linux's cap on one call */

static int seekable(struct file *f)
{
    return f && f->ops && f->ops->lseek && !pipe_is(f);
}

/*
 * Up to `len` bytes from `in` to `out`. A null offset means the file's
 * own position, moved; a given one is used and advanced instead, the
 * position left alone. `nonblock` refuses to wait on a pipe.
 */
static s32 xfer(int in, u32 *in_off, int out, u32 *out_off, u32 len,
                int nonblock)
{
    struct file *fi = fd_get(in);
    u8 *buf;
    s32 total = 0;

    if (len > XFER_MAX) {
        len = XFER_MAX;
    }
    buf = (u8 *)pmm_alloc();
    if (!buf) {
        return -ENOMEM;
    }
    while ((u32)total < len) {
        u32 want = len - (u32)total;
        s32 got, put = 0;

        if (want > PAGE_SIZE) {
            want = PAGE_SIZE;
        }
        if ((total > 0 || nonblock) && !seekable(fi) &&
            !(poll_fd(in) & (POLLIN | POLLHUP))) {
            if (total == 0) {
                total = -EAGAIN;
            }
            break;
        }
        got = in_off ? fd_pread(in, buf, want, *in_off)
                     : fd_read(in, buf, want);
        if (got <= 0) {
            if (total == 0) {
                total = got;            /* 0 at the end, or -errno */
            }
            break;
        }
        while (put < got) {
            s32 w;

            if (nonblock && pipe_is(fd_get(out)) &&
                !(poll_fd(out) & POLLOUT)) {
                w = put ? 0 : -EAGAIN;
            } else if (out_off) {
                w = fd_pwrite(out, buf + put, (u32)(got - put),
                              *out_off + (u32)put);
            } else {
                w = fd_write(out, buf + put, (u32)(got - put));
            }
            if (w <= 0) {
                if (put == 0 && total == 0) {
                    total = w ? w : -EIO;
                }
                break;
            }
            put += w;
        }
        if (put > 0) {
            total += put;
            if (in_off) {
                *in_off += (u32)put;
            }
            if (out_off) {
                *out_off += (u32)put;
            }
        }
        /* What was read and not written goes back to the input, where
         * it can: a file's position is wound back; a pipe's bytes are
         * gone, as they are when Linux's splice meets a short write. */
        if (put < got) {
            if (!in_off && seekable(fi)) {
                fd_lseek(in, put - got, SEEK_CUR);
            }
            break;
        }
        if ((u32)got < want && seekable(fi)) {
            break;                      /* the end of the file */
        }
    }
    pmm_free((u32)buf);
    return total;
}

/* A 32-bit or 64-bit user offset, in and out. */
static int get_off(u32 u, int wide, u32 *off)
{
    int err;

    if (wide) {
        s64 v;

        if ((err = fetch(&v, u, sizeof(v))) < 0) {
            return err;
        }
        if (v < 0) {
            return -EINVAL;
        }
        if (v >> 32) {
            return -EOVERFLOW;
        }
        *off = (u32)v;
    } else {
        s32 v;

        if ((err = fetch(&v, u, sizeof(v))) < 0) {
            return err;
        }
        if (v < 0) {
            return -EINVAL;
        }
        *off = (u32)v;
    }
    return 0;
}

static int put_off(u32 u, int wide, u32 off)
{
    if (wide) {
        s64 v = off;

        return sys_store(u, &v, sizeof(v));
    } else {
        s32 v = (s32)off;

        return sys_store(u, &v, sizeof(v));
    }
}

static int readable(struct file *f)
{
    return (f->flags & O_ACCMODE) != O_WRONLY;
}

static int writable(struct file *f)
{
    return (f->flags & O_ACCMODE) != O_RDONLY;
}

s32 sys_sendfile(int out, int in, u32 uoff, u32 count, int wide)
{
    struct file *fi = fd_get(in), *fo = fd_get(out);
    u32 off;
    s32 r;
    int err;

    if (!fi || !fo || !readable(fi) || !writable(fo)) {
        return -EBADF;
    }
    if ((fo->flags & O_APPEND)) {
        return -EINVAL;             /* Linux's rule */
    }
    if (!uoff) {
        return xfer(in, 0, out, 0, count, 0);
    }
    if (!seekable(fi)) {
        return -ESPIPE;
    }
    if ((err = get_off(uoff, wide, &off)) < 0) {
        return err;
    }
    r = xfer(in, &off, out, 0, count, 0);
    if (r >= 0 && (err = put_off(uoff, wide, off)) < 0) {
        return err;
    }
    return r;
}

s32 sys_splice(int in, u32 uoff_in, int out, u32 uoff_out, u32 len,
               u32 flags)
{
    struct file *fi = fd_get(in), *fo = fd_get(out);
    u32 oi = 0, oo = 0;
    s32 r;
    int err;

    if (!fi || !fo || !readable(fi) || !writable(fo)) {
        return -EBADF;
    }
    if (flags & ~(SPLICE_F_MOVE | SPLICE_F_NONBLOCK | SPLICE_F_MORE |
                  SPLICE_F_GIFT)) {
        return -EINVAL;
    }
    /* One end must be a pipe -- splice is "to or from a pipe" -- and a
     * pipe has no offset. */
    if (!pipe_is(fi) && !pipe_is(fo)) {
        return -EINVAL;
    }
    if ((uoff_in && pipe_is(fi)) || (uoff_out && pipe_is(fo))) {
        return -ESPIPE;
    }
    if (fi == fo) {
        return -EINVAL;
    }
    if ((uoff_in && (err = get_off(uoff_in, 1, &oi)) < 0) ||
        (uoff_out && (err = get_off(uoff_out, 1, &oo)) < 0)) {
        return err;
    }
    if (len == 0) {
        return 0;
    }
    r = xfer(in, uoff_in ? &oi : 0, out, uoff_out ? &oo : 0, len,
             (flags & SPLICE_F_NONBLOCK) != 0);
    if (r > 0 && ((uoff_in && (err = put_off(uoff_in, 1, oi)) < 0) ||
                  (uoff_out && (err = put_off(uoff_out, 1, oo)) < 0))) {
        return err;
    }
    return r;
}

s32 sys_copy_file_range(int in, u32 uoff_in, int out, u32 uoff_out,
                        u32 len, u32 flags)
{
    struct file *fi = fd_get(in), *fo = fd_get(out);
    struct stat si, so;
    u32 oi = 0, oo = 0;
    s32 r;
    int err;

    if (!fi || !fo || !readable(fi) || !writable(fo) ||
        (fo->flags & O_APPEND)) {
        return -EBADF;
    }
    if (flags) {
        return -EINVAL;
    }
    if (vfs_file_stat(fi, &si) < 0 || vfs_file_stat(fo, &so) < 0) {
        return -EINVAL;
    }
    if (S_ISDIR(si.st_mode) || S_ISDIR(so.st_mode)) {
        return -EISDIR;
    }
    if (!S_ISREG(si.st_mode) || !S_ISREG(so.st_mode)) {
        return -EINVAL;
    }
    if ((uoff_in && (err = get_off(uoff_in, 1, &oi)) < 0) ||
        (uoff_out && (err = get_off(uoff_out, 1, &oo)) < 0)) {
        return err;
    }
    /* The same file, overlapping ranges: Linux refuses. */
    if (si.st_ino == so.st_ino) {
        u32 ai = uoff_in ? oi : fi->pos, ao = uoff_out ? oo : fo->pos;

        if (ai < ao + len && ao < ai + len) {
            return -EINVAL;
        }
    }
    r = xfer(in, uoff_in ? &oi : 0, out, uoff_out ? &oo : 0, len, 0);
    if (r > 0 && ((uoff_in && (err = put_off(uoff_in, 1, oi)) < 0) ||
                  (uoff_out && (err = put_off(uoff_out, 1, oo)) < 0))) {
        return err;
    }
    return r;
}
