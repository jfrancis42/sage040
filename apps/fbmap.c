/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fbmap - drawing into /dev/fb0 as memory.
 *
 * mmap the framebuffer, write pixels as bytes, and have them appear:
 * the harness (devtest.sh) takes a screenshot through the monitor and
 * looks. A child made by fork draws through the mapping it inherited,
 * which is the same video memory, not a copy of it. And mapping 16 MB
 * of video memory takes no RAM, and giving it back gives none back.
 *
 *   red   rectangle at (0,0)-(199,99), drawn by the parent
 *   green rectangle at (300,0)-(399,99), drawn by the child
 */
#include "ulib.h"

#define RED     200
#define GREEN   201

static int failures;

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
    if (!ok) {
        failures++;
    }
}

static void say(const char *what, u32 v)
{
    puts("fbmap: ");
    puts(what);
    putch(' ');
    puthex(v);
    putch('\n');
}

static u32 free_pages(void)
{
    struct sysinfo si;

    return sysinfo(&si) < 0 ? 0 : si.freeram;
}

static void box(u8 *fb, u32 pitch, u32 x0, u32 y0, u32 w, u32 h, u8 c)
{
    u32 x, y;

    for (y = y0; y < y0 + h; y++) {
        for (x = x0; x < x0 + w; x++) {
            fb[y * pitch + x] = c;
        }
    }
}

int main(void)
{
    struct fb_info info;
    struct fb_palette pal;
    u8 *fb;
    u32 before, len;
    int fd, pid, st;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        puts("fbmap: no /dev/fb0\n");
        return 1;
    }
    ioctl(fd, FBIO_DOUBLE, 0);              /* draw where it is shown */
    ioctl(fd, FBIO_CLEAR, 0);
    ioctl(fd, FBIO_SYNC, 0);
    memset(&info, 0, sizeof(info));
    ioctl(fd, FBIO_GETINFO, (u32)&info);
    report("FBIO_GETINFO says how much video memory there is, and where",
           info.mem_size >= info.pitch * info.height &&
           info.draw_offset == info.show_offset);
    report("  and this is 8 bits a pixel, which the rest assumes", info.bpp == 8);

    pal.index = RED;
    pal.rgb = 0xff0000;
    ioctl(fd, FBIO_PALETTE, (u32)&pal);
    pal.index = GREEN;
    pal.rgb = 0x00ff00;
    ioctl(fd, FBIO_PALETTE, (u32)&pal);

    before = free_pages();
    len = info.mem_size;
    fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    report("mmap of all of video memory works", fb != MAP_FAILED);
    if (fb == MAP_FAILED) {
        return 1;
    }
    say("pages it cost:", before - free_pages());
    /* Its page tables are made now: a 256-byte table per 64 pages, in
     * 512-byte slots, 8 to a page -- 9 pages or so for 16 MB. None of
     * the 4096 pages it maps. */
    report("  and takes no RAM for it, only its page tables",
           before - free_pages() <= 16);
    {
        struct pageinfo pi;

        memset(&pi, 0, sizeof(pi));
        syscall(__NR_memctl, MEMCTL_PAGE, (u32)fb, (u32)&pi);
        say("physical address behind the mapping:", pi.pa);
        report("  and the mapping IS the video memory, not a copy of it",
               pi.pa == 0xf0000000UL);
    }
    fb += info.draw_offset;

    box(fb, info.pitch, 0, 0, 200, 100, RED);

    pid = fork();
    if (pid == 0) {
        box(fb, info.pitch, 300, 0, 100, 100, GREEN);
        exit(0);
    }
    waitpid(pid, &st, 0);

    report("what was written reads back from video memory",
           fb[10 * info.pitch + 10] == RED && fb[99 * info.pitch + 199] == RED);
    report("  and so does what the forked child wrote through its copy of the mapping",
           fb[10 * info.pitch + 310] == GREEN);

    munmap(fb - info.draw_offset, len);
    report("munmap gives back no RAM that was never taken",
           free_pages() <= before && before - free_pages() <= 16);
    close(fd);

    puts("fbmap: ");
    putdec((u32)failures);
    puts(" failed\n");
    return failures != 0;
}
