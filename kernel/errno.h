/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * errno.h - error numbers.
 *
 * The values are Linux's, and so are the names. Nothing here needs them
 * to be -- the kernel could number its own errors however it liked --
 * but anyone who has written C on Linux already knows what -ENOENT and
 * -EBADF mean, and a system that answers -22 where Linux answers -22 is
 * one less thing to learn.
 *
 * The convention is Linux's too: a function returns a non-negative
 * result on success, or the negated error on failure. There is no global
 * errno, which is a variable that only makes sense once there are
 * threads to get it wrong.
 */
#ifndef ERRNO_H
#define ERRNO_H

#define EPERM            1      /* operation not permitted            */
#define ENOENT           2      /* no such file or directory          */
#define ESRCH            3      /* no such process                    */
#define EINTR            4      /* interrupted by a signal            */
#define EIO              5      /* I/O error                          */
#define ENXIO            6      /* no such device or address          */
#define E2BIG            7      /* argument list too long             */
#define ENOEXEC          8      /* exec format error                  */
#define EBADF            9      /* bad file descriptor                */
#define ECHILD          10      /* no child processes                 */
#define EAGAIN          11      /* try again                          */
#define ENOMEM          12      /* out of memory                      */
#define EACCES          13      /* permission denied                  */
#define EFAULT          14      /* bad address                        */
#define EBUSY           16      /* device or resource busy            */
#define EEXIST          17      /* file exists                        */
#define EXDEV           18      /* cross-device link                  */
#define ENODEV          19      /* no such device                     */
#define ENOTDIR         20      /* not a directory                    */
#define EISDIR          21      /* is a directory                     */
#define EINVAL          22      /* invalid argument                   */
#define ENFILE          23      /* file table overflow                */
#define EMFILE          24      /* too many open files                */
#define ENOTTY          25      /* not a typewriter                   */
#define EFBIG           27      /* file too large                     */
#define ENOSPC          28      /* no space left on device            */
#define ESPIPE          29      /* illegal seek                       */
#define EROFS           30      /* read-only file system              */
#define ERANGE          34      /* result too large                   */
#define ENAMETOOLONG    36      /* file name too long                 */
#define ENOSYS          38      /* function not implemented           */
#define ENOTEMPTY       39      /* directory not empty                */
#define EPIPE           32      /* broken pipe                        */
#define ENOTSOCK        88      /* socket operation on non-socket     */
#define EDESTADDRREQ    89      /* destination address required       */
#define EPROTONOSUPPORT 93      /* protocol not supported             */
#define EOPNOTSUPP      95      /* operation not supported            */
#define EAFNOSUPPORT    97      /* address family not supported       */
#define EADDRINUSE      98      /* address already in use             */
#define EMSGSIZE        90      /* message too long                   */
#define ENOPROTOOPT     92      /* protocol not available (option)    */
#define EADDRNOTAVAIL   99      /* cannot assign requested address    */
#define ENETDOWN       100      /* network is down                    */
#define ENETUNREACH    101      /* network is unreachable             */
#define ECONNRESET     104      /* connection reset by peer           */
#define ETIMEDOUT      110      /* connection timed out               */
#define ECONNREFUSED   111      /* connection refused                 */
#define EALREADY       114      /* already in progress                */
#define EINPROGRESS    115      /* now in progress                    */
#define ENOBUFS        105      /* no buffer space available          */
#define EISCONN        106      /* already connected                  */
#define ENOTCONN       107      /* not connected                      */
#define EHOSTUNREACH   113      /* no route to host                   */
#define ENOMEDIUM       123     /* no medium found                    */

const char *strerror(int err);  /* takes either sign */

#endif /* ERRNO_H */
