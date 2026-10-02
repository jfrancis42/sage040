/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ptest - ask the MMU what it makes of an address, and check the
 * kernel's own walk agrees.
 *
 *     ptest [-p PID] [-s] ADDR...
 *
 * For each address, the 68040's own answer (PTESTR, then MMUSR) and
 * the page descriptor the kernel's software walk finds, side by side,
 * and whether the two agree: resident or not, the same physical page,
 * the same write protection and supervisor bit. Two independent answers
 * to one question, so a descriptor changed without a flush, or a table
 * the MMU reads differently from the kernel, shows as a DISAGREE line
 * rather than as some later fault nobody can trace.
 *
 * -p PID asks about another process's address space; -s about the
 * supervisor's map. Both are root's. Exit status 1 if any address
 * disagreed, 2 for a usage or system call error.
 */
#include "ulib.h"

static int hexval(const char *s, u32 *out)
{
    u32 v = 0;
    int any = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (; *s; s++, any = 1) {
        int d;

        if (*s >= '0' && *s <= '9') {
            d = *s - '0';
        } else if (*s >= 'a' && *s <= 'f') {
            d = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'F') {
            d = *s - 'A' + 10;
        } else {
            return 0;
        }
        v = (v << 4) | (u32)d;
    }
    *out = v;
    return any;
}

static int decval(const char *s, u32 *out)
{
    u32 v = 0;

    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10 + (u32)(*s - '0');
    }
    *out = v;
    return 1;
}

static void flags(u32 wp, u32 s, u32 m)
{
    puts(wp ? " ro" : " rw");
    puts(s ? " super" : " user");
    if (m) {
        puts(" modified");
    }
}

int main(int argc, char **argv)
{
    struct ptestinfo pt;
    u32 pid = 0, super = 0, va;
    int i, bad = 0, any = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            if (!decval(argv[++i], &pid)) {
                eputs("ptest: -p wants a process id\n");
                return 2;
            }
        } else if (strcmp(argv[i], "-s") == 0) {
            super = 1;
        } else {
            break;
        }
    }
    if (i >= argc) {
        eputs("usage: ptest [-p PID] [-s] ADDR...\n");
        return 2;
    }
    for (; i < argc; i++) {
        s32 r;
        u32 mpa, wpa, mres, wres;
        int agree;

        if (!hexval(argv[i], &va)) {
            eputs("ptest: not a hex address: ");
            eputs(argv[i]);
            eputs("\n");
            return 2;
        }
        memset(&pt, 0, sizeof(pt));
        pt.pid = pid;
        pt.va = va;
        pt.super = super;
        r = syscall(__NR_memctl, MEMCTL_PTEST, sizeof(pt), (u32)&pt);
        if (r < 0) {
            eputs(r == -EPERM ? "ptest: another's address space is root's\n" :
                  r == -ESRCH ? "ptest: no such process\n" :
                  r == -ENODEV ? "ptest: that task has no user address space\n" :
                                 "ptest: memctl failed\n");
            return 2;
        }
        any = 1;
        puthex(va);
        puts("\n  mmu : ");
        mres = pt.mmusr & MMUSR_R;
        mpa = pt.mmusr & MMUSR_PA;
        if (pt.mmusr & MMUSR_B) {
            puts("bus error walking the tables");
        } else if (pt.mmusr & MMUSR_T) {
            puts("transparent translation (no tables consulted)");
        } else if (mres) {
            puts("resident, page ");
            puthex(mpa);
            flags(pt.mmusr & MMUSR_WP, pt.mmusr & MMUSR_S, pt.mmusr & MMUSR_M);
        } else {
            puts("not resident");
        }
        puts("\n  walk: ");
        wres = pt.desc & 1;             /* PDT 01 or 11: resident */
        wpa = pt.desc & 0xfffff000;
        if (!pt.desc) {
            puts("no table reaches it");
        } else if (wres) {
            puts("resident, page ");
            puthex(wpa);
            flags(pt.desc & 0x004, pt.desc & 0x080, pt.desc & 0x010);
        } else {
            puts("invalid descriptor ");
            puthex(pt.desc);
            puts(" (the kernel's own state: lazy, swapped, or none)");
        }
        if (pt.mmusr & (MMUSR_T | MMUSR_B)) {
            agree = 1;                  /* nothing the walk can say */
        } else if (mres) {
            agree = wres && mpa == wpa &&
                    !(pt.mmusr & MMUSR_WP) == !(pt.desc & 0x004) &&
                    !(pt.mmusr & MMUSR_S) == !(pt.desc & 0x080);
        } else {
            agree = !wres;
        }
        puts(agree ? "\n  agree\n" : "\n  DISAGREE\n");
        bad |= !agree;
    }
    return any && !bad ? 0 : 1;
}
