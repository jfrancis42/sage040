/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * string.c - memory and string primitives.
 *
 * Byte at a time throughout.  A 68040 moves longwords considerably
 * faster, but the copies here are directory entries and short buffers,
 * and a wrong fast version would be a very hard bug to find in a
 * filesystem.  Worth revisiting when something measures as slow.
 */
#include "string.h"

void *memcpy(void *dst, const void *src, ksize_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, ksize_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, ksize_t n)
{
    u8 *d = dst;

    while (n--) {
        *d++ = (u8)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, ksize_t n)
{
    const u8 *p = a;
    const u8 *q = b;

    while (n--) {
        if (*p != *q) {
            return (int)*p - (int)*q;
        }
        p++;
        q++;
    }
    return 0;
}

ksize_t strlen(const char *s)
{
    const char *p = s;

    while (*p) {
        p++;
    }
    return (ksize_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, ksize_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != '\0') {
        /* copy including the terminator */
    }
    return dst;
}

/* Case-insensitive compare, for command and file names typed at the
 * console -- MS-DOS names are not case sensitive. */
int stricmp(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) {
            return (int)(u8)ca - (int)(u8)cb;
        }
        if (ca == '\0') {
            return 0;
        }
    }
}

char *strncpy(char *dst, const char *src, ksize_t n)
{
    ksize_t i;

    for (i = 0; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    /* The standard pads the rest with NULs rather than stopping, which
     * is the part everybody forgets and the part that makes it safe to
     * use on a fixed buffer that will be compared byte for byte. */
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
    }
    return c == '\0' ? (char *)s : 0;
}
