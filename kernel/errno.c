/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * errno.c - error numbers as words.
 *
 * The wording is Linux's, near enough that someone who has seen the
 * message before recognises it. Anything not listed prints its number,
 * which is more use than "unknown error" on its own.
 */
#include "errno.h"
#include "kernel.h"

const char *strerror(int err)
{
    if (err < 0) {
        err = -err;
    }
    switch (err) {
    case 0:               return "success";
    case EPERM:           return "operation not permitted";
    case ENOENT:          return "no such file or directory";
    case EINTR:           return "interrupted system call";
    case EIO:             return "input/output error";
    case ENXIO:           return "no such device or address";
    case E2BIG:           return "argument list too long";
    case ENOEXEC:         return "exec format error";
    case EBADF:           return "bad file descriptor";
    case ECHILD:          return "no child processes";
    case EAGAIN:          return "resource temporarily unavailable";
    case ENOMEM:          return "cannot allocate memory";
    case EACCES:          return "permission denied";
    case EFAULT:          return "bad address";
    case EBUSY:           return "device or resource busy";
    case EEXIST:          return "file exists";
    case EXDEV:           return "invalid cross-device link";
    case ENODEV:          return "no such device";
    case ENOTDIR:         return "not a directory";
    case EISDIR:          return "is a directory";
    case EINVAL:          return "invalid argument";
    case ENFILE:          return "too many open files in system";
    case EMFILE:          return "too many open files";
    case ENOTTY:          return "inappropriate ioctl for device";
    case EFBIG:           return "file too large";
    case ENOSPC:          return "no space left on device";
    case ESPIPE:          return "illegal seek";
    case EROFS:           return "read-only file system";
    case ENAMETOOLONG:    return "file name too long";
    case ENOSYS:          return "function not implemented";
    case ENOTEMPTY:       return "directory not empty";
    case EPIPE:           return "broken pipe";
    case ENOTSOCK:        return "socket operation on non-socket";
    case EDESTADDRREQ:    return "destination address required";
    case EPROTONOSUPPORT: return "protocol not supported";
    case EOPNOTSUPP:      return "operation not supported";
    case EAFNOSUPPORT:    return "address family not supported";
    case ENOBUFS:         return "no buffer space available";
    case EISCONN:         return "transport endpoint is already connected";
    case ENOTCONN:        return "transport endpoint is not connected";
    case EADDRINUSE:      return "address already in use";
    case EMSGSIZE:        return "message too long";
    case EADDRNOTAVAIL:   return "cannot assign requested address";
    case ENETDOWN:        return "network is down";
    case ENETUNREACH:     return "network is unreachable";
    case ECONNRESET:      return "connection reset by peer";
    case ETIMEDOUT:       return "connection timed out";
    case ECONNREFUSED:    return "connection refused";
    case EHOSTUNREACH:    return "no route to host";
    case ENOMEDIUM:       return "no medium found";
    default:              return "unknown error";
    }
}
