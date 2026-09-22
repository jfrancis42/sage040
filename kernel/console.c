/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * console.c - the kernel's own output path.
 *
 * One indirection: a pointer to whichever character device was
 * registered as the console. It does not know what that device is, so
 * replacing the NS16550A with another UART, or with a framebuffer
 * terminal, changes nothing here and nothing in any caller.
 *
 * Before a console is registered these are silent. Nothing prints
 * before main.c brings the console up, which it does first for exactly
 * this reason.
 */
#include "console.h"
#include "vfs.h"
#include "errno.h"
#include "klog.h"

static struct chardev *con;
static struct file confile;

void console_set(struct chardev *d)
{
    con = d;
    confile.ops = d ? d->ops : 0;
    confile.priv = d ? d->priv : 0;
    confile.used = 1;
    confile.flags = 0;
}

struct chardev *console_get(void)
{
    return con;
}

/*
 * Move the console. Kernel messages and the standard descriptors go
 * together, because a program's output and the kernel's belong on the
 * same screen.
 *
 * Descriptor 0 moves too, even though on this machine every device that
 * can be a console reads from the same serial line: the framebuffer
 * console forwards reads to it. Binding all three keeps the rule simple
 * -- your console is one device -- rather than making 0 a special case
 * that is right until something else can type.
 */
int console_use(struct chardev *d)
{
    if (!d || !d->ops) {
        return -EINVAL;
    }
    console_set(d);
    fd_bind(0, d->ops, d->priv, O_RDONLY);
    fd_bind(1, d->ops, d->priv, O_WRONLY);
    fd_bind(2, d->ops, d->priv, O_WRONLY);
    return 0;
}

/*
 * Everything the kernel prints goes two places: the console, where a
 * person sees it, and the log ring, where a program can read it back
 * afterwards (klog.c). The ring comes FIRST, so that a message is
 * recorded even if the console write is what goes wrong.
 */
void console_write(const void *buf, u32 len)
{
    klog_write(buf, len);
    if (!con || !con->ops->write) {
        return;
    }
    con->ops->write(&confile, buf, len);
}

void kputc(char c)
{
    klog_putc(c);
    if (!con || !con->ops->write) {
        return;
    }
    /*
     * The newline goes out as a newline. Turning it into a carriage
     * return and a line feed is the terminal driver's job -- ONLCR, in
     * the language of a real tty -- and doing it here as well would put
     * two of them on the wire.
     */
    con->ops->write(&confile, &c, 1);
}

void kputs(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    klog_write(s, n);
    if (!con || !con->ops->write) {
        return;
    }
    con->ops->write(&confile, s, n);
}

void kputln(const char *s)
{
    kputs(s);
    kputc('\n');
}

static const char hexdigits[] = "0123456789abcdef";

void kputhex8(u8 v)
{
    kputc(hexdigits[(v >> 4) & 0xf]);
    kputc(hexdigits[v & 0xf]);
}

void kputhex16(u16 v)
{
    kputhex8((u8)(v >> 8));
    kputhex8((u8)v);
}

void kputhex32(u32 v)
{
    kputhex16((u16)(v >> 16));
    kputhex16((u16)v);
}

void kputdec(u32 v)
{
    char buf[12];
    int i = 0;

    if (v == 0) {
        kputc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        kputc(buf[--i]);
    }
}

void kput2(u32 v)
{
    kputc((char)('0' + (v / 10) % 10));
    kputc((char)('0' + v % 10));
}
