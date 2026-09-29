/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * faulttest - a program's own faults, caught and survived.
 *
 * Driven by kernel/faulttest.sh. Every fault here is one the program
 * asked to hear about, and every handler either repairs the cause and
 * returns -- so the faulting instruction must run again and succeed --
 * or moves the program on through its ucontext. Each prints ok or FAIL.
 *
 *   1  read of a PROT_NONE page        SIGSEGV, si_addr; mprotect, return
 *   2  write to a read-only page       SIGSEGV SEGV_ACCERR; the write lands
 *   3  illegal instruction            SIGILL ILL_ILLOPC at its address
 *   4  divide by zero                  SIGFPE FPE_INTDIV, then on
 *   5  TRAP #15                        SIGTRAP TRAP_BRKPT
 *   6  an old-style handler            SIGSEGV repaired without siginfo
 *   7  SIGSEGV blocked                 still the end of the program
 *   8  write to the program's own code SIGSEGV SEGV_ACCERR: text is read
 *                                      only; mprotect, and the write lands
 *   9  write to a string literal       SIGSEGV too: .rodata is text's
 *
 * Not here: SIGBUS. On a real 68040 a jump to an odd address is an
 * address error, and trap.c makes it SIGBUS BUS_ADRALN; QEMU raises its
 * address error only for an invalid addressing mode and runs code at an
 * odd address without complaint, so there is nothing to catch.
 */
#include "ulib.h"

static int failures;
static volatile int hits;
static volatile u32 got_addr;
static volatile int got_code, got_sig;
static u8 *page;

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
    if (!ok) {
        failures++;
    }
}

static void remember(int sig, const struct siginfo *si)
{
    hits++;
    got_sig = sig;
    got_code = si->si_code;
    got_addr = si->_sifields._sigfault.si_addr;
}

/* 1, 2: make the page usable, and return to the faulting instruction. */
static void on_segv(int sig, struct siginfo *si, void *uc)
{
    (void)uc;
    remember(sig, si);
    mprotect(page, 4096, PROT_READ | PROT_WRITE);
}

/* 3: step over the two-byte ILLEGAL. */
static void on_ill(int sig, struct siginfo *si, void *ucp)
{
    struct ucontext *uc = ucp;

    remember(sig, si);
    uc->uc_mcontext.gregs[16] += 2;
}

/* 4, 5: the frame's PC is already past the instruction. */
static void on_note(int sig, struct siginfo *si, void *uc)
{
    (void)uc;
    remember(sig, si);
}

/* 8, 9: make the page of the faulting address writable, and return. */
static void on_segv_text(int sig, struct siginfo *si, void *uc)
{
    (void)uc;
    remember(sig, si);
    mprotect((void *)(si->_sifields._sigfault.si_addr & ~4095UL), 4096,
             PROT_READ | PROT_WRITE);
}

/* Something to write over: never called. */
static void __attribute__((noinline)) victim(void)
{
    __asm__ volatile("nop");
}

static const char literal[] = "a string the program may not change";

/* 6: no siginfo; the page is repaired all the same. */
static void on_segv_old(int sig)
{
    (void)sig;
    hits++;
    mprotect(page, 4096, PROT_READ | PROT_WRITE);
}

static void catch3(int sig, void (*h)(int, struct siginfo *, void *))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (sighandler_t)(void *)h;
    sa.sa_flags = SA_SIGINFO;
    sigaction(sig, &sa, 0);
}

int main(void)
{
    volatile u32 *p;
    u32 v;
    int st, pid;

    page = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        puts("faulttest: no page\n");
        return 1;
    }
    p = (volatile u32 *)page;

    /* 1 */
    catch3(SIGSEGV, on_segv);
    hits = 0;
    v = p[1];
    report("reading a PROT_NONE page is SIGSEGV, and the retried read works",
           hits == 1 && got_sig == SIGSEGV && v == 0);
    report("  and si_addr is the address read",
           got_addr == (u32)&p[1]);

    /* 2: resident now, and read-only */
    mprotect(page, 4096, PROT_READ);
    hits = 0;
    p[2] = 0x1234abcd;
    report("writing a read-only page is SIGSEGV, and the retried write lands",
           hits == 1 && p[2] == 0x1234abcd);
    report("  and it is SEGV_ACCERR, at the address written",
           got_code == SEGV_ACCERR && got_addr == (u32)&p[2]);

    /* 3 */
    catch3(SIGILL, on_ill);
    hits = 0;
    __asm__ volatile("illegal");
    report("an illegal instruction is SIGILL ILL_ILLOPC, and stepping over it works",
           hits == 1 && got_sig == SIGILL && got_code == ILL_ILLOPC);

    /* 4 */
    catch3(SIGFPE, on_note);
    hits = 0;
    {
        u32 n = 10, d = 0;

        __asm__ volatile("divu.w %1,%0" : "+d"(n) : "d"(d));
    }
    report("divide by zero is SIGFPE FPE_INTDIV, and the program carries on",
           hits == 1 && got_sig == SIGFPE && got_code == FPE_INTDIV);

    /* 5 */
    catch3(SIGTRAP, on_note);
    hits = 0;
    __asm__ volatile("trap #15");
    report("TRAP #15 is SIGTRAP TRAP_BRKPT",
           hits == 1 && got_sig == SIGTRAP && got_code == TRAP_BRKPT);

    /* 6 */
    mprotect(page, 4096, PROT_NONE);
    signal(SIGSEGV, on_segv_old);
    hits = 0;
    v = p[3];
    report("an old-style handler repairs a SIGSEGV too", hits == 1 && v == 0);

    /* 7: a child that blocks SIGSEGV and faults anyway */
    pid = fork();
    if (pid == 0) {
        sigset_t s;

        signal(SIGSEGV, on_segv_old);
        sigemptyset(&s);
        sigaddset(&s, SIGSEGV);
        sigprocmask(SIG_BLOCK, &s, 0);
        mprotect(page, 4096, PROT_NONE);
        v = p[0];
        puts("faulttest: a blocked SIGSEGV let the program go on\n");
        exit(0);
    }
    st = 0;
    waitpid(pid, &st, 0);
    report("a blocked SIGSEGV still ends the program",
           pid > 0 && WTERMSIG(st) == SIGSEGV);

    /* 8 */
    catch3(SIGSEGV, on_segv_text);
    hits = 0;
    *(volatile u16 *)(void *)victim = 0x4e71;       /* nop over its nop */
    report("writing the program's own code is SIGSEGV SEGV_ACCERR, there",
           hits == 1 && got_code == SEGV_ACCERR &&
           got_addr == (u32)(void *)victim);
    report("  and after mprotect the write lands",
           *(volatile u16 *)(void *)victim == 0x4e71);
    /* Read-only again: the literal below may well share the page. */
    mprotect((void *)((u32)(void *)victim & ~4095UL), 4096, PROT_READ);

    /* 9 */
    hits = 0;
    *(volatile char *)&literal[0] = 'A';
    report("writing a string literal is SIGSEGV too",
           hits == 1 && got_code == SEGV_ACCERR &&
           *(volatile const char *)&literal[0] == 'A');

    puts(failures ? "faulttest: FAILED\n" : "faulttest: all right\n");
    return failures != 0;
}
