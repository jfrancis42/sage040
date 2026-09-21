/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sigtest - the system call gate, and signals.
 *
 * First, that a system call hands every register back as it found it
 * except d0, which carries the result. The gate saves all fifteen and
 * a signal handler is started by rewriting them, so this is the ground
 * everything else here stands on. Then handlers, masks, restarting, and
 * everything the interrupted code had surviving a handler that clobbers
 * all of it.
 *
 * Modes, for what only the shell or a second task can see:
 *
 *   poke PID SIG MS   sleep MS, then send SIG to PID, then exit
 *   nap MS            sleep MS and say how long it really took
 *   catchint          wait in pause() for ctrl-C, and catch it
 *   spincatch         compute without system calls until ctrl-C
 *   badstack          take a signal with an unusable stack
 *   forge             sigreturn to a forged context asking for
 *                     supervisor mode
 */
#include "ulib.h"

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

/*
 * Load a known value into every register the ABI lets a call keep,
 * make a call, and store them all back. d0 is the call number going in
 * and the result coming out; d1 is getpid's (unused) first argument and
 * must survive too.
 */
static u32 after[14];

static s32 regs_across_a_call(void)
{
    register u32 d0 __asm__("d0") = __NR_getpid;
    register u32 *out __asm__("a1") = after;

    __asm__ volatile (
        "movem.l %%d2-%%d7/%%a2-%%a6,-(%%sp)\n\t"
        "move.l  %%a1,-(%%sp)\n\t"
        "move.l  #0x11111111,%%d1\n\t"
        "move.l  #0x22222222,%%d2\n\t"
        "move.l  #0x33333333,%%d3\n\t"
        "move.l  #0x44444444,%%d4\n\t"
        "move.l  #0x55555555,%%d5\n\t"
        "move.l  #0x66666666,%%d6\n\t"
        "move.l  #0x77777777,%%d7\n\t"
        "move.l  #0x10000000,%%a0\n\t"
        "move.l  #0x10000001,%%a1\n\t"
        "move.l  #0x10000002,%%a2\n\t"
        "move.l  #0x10000003,%%a3\n\t"
        "move.l  #0x10000004,%%a4\n\t"
        "move.l  #0x10000005,%%a5\n\t"
        "move.l  #0x10000006,%%a6\n\t"
        "trap    #0\n\t"
        "move.l  %%a1,-(%%sp)\n\t"          /* free a1 to reach `after` */
        "move.l  4(%%sp),%%a1\n\t"
        "movem.l %%d1-%%d7/%%a0,(%%a1)\n\t" /* 8 longs */
        "move.l  (%%sp)+,32(%%a1)\n\t"      /* the saved a1 */
        "movem.l %%a2-%%a6,36(%%a1)\n\t"    /* 5 longs, to 56 */
        "addq.l  #4,%%sp\n\t"
        "movem.l (%%sp)+,%%d2-%%d7/%%a2-%%a6\n\t"
        : "+d"(d0), "+a"(out)
        :
        : "d1", "a0", "memory", "cc");
    return (s32)d0;
}

static void test_gate(void)
{
    static const u32 want[14] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
        0x55555555, 0x66666666, 0x77777777,
        0x10000000, 0x10000001, 0x10000002, 0x10000003,
        0x10000004, 0x10000005, 0x10000006,
    };
    s32 r = regs_across_a_call();
    int i, ok = 1;

    report("getpid through a hand-written trap returns the pid",
           r == syscall(__NR_getpid));
    for (i = 0; i < 14; i++) {
        ok &= after[i] == want[i];
    }
    report("  and d1-d7, a0-a6 all came back unchanged", ok);
}


/* ---------------------------------------------------------------- */

static char *self = "/SIGTEST";

static void utoa(char *out, u32 v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *out++ = tmp[--n];
    }
    *out = '\0';
}

static u32 atou(const char *s)
{
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s++ - '0');
    }
    return v;
}
static volatile int count[NSIG];
static volatile int depth, max_depth;
static char order[16];
static volatile int order_n;

static void note(char c)
{
    if (order_n < (int)sizeof(order) - 1) {
        order[order_n++] = c;
        order[order_n] = '\0';
    }
}

static u32 now_ms(void)
{
    return times() * (1000 / HZ);
}

static void catch(int sig, sighandler_t h, u32 flags, sigset_t mask)
{
    struct sigaction act;

    act.sa_handler = h;
    act.sa_mask = mask;
    act.sa_flags = flags;
    act.sa_restorer = 0;
    sigaction(sig, &act, 0);
}

static void counting(int sig)
{
    count[sig]++;
}

static void nesting(int sig)
{
    depth++;
    if (depth > max_depth) {
        max_depth = depth;
    }
    count[sig]++;
    if (count[sig] == 1) {
        raise(sig);             /* again, from inside its own handler */
    }
    depth--;
}

static void usr1_then_usr2(int sig)
{
    note('1');
    raise(SIGUSR2);             /* blocked by sa_mask while in here */
    note('1');
    (void)sig;
}

static void usr2_note(int sig)
{
    note('2');
    (void)sig;
}

/* Wrecks every register a handler can reach, integer and FP. */
static void clobber(int sig)
{
    count[sig]++;
    __asm__ volatile (
        "move.l  #0xdeadbeef,%%d0\n\t"
        "move.l  %%d0,%%d1\n\t"
        "move.l  %%d0,%%d2\n\t"
        "move.l  %%d0,%%d3\n\t"
        "move.l  %%d0,%%d4\n\t"
        "move.l  %%d0,%%d5\n\t"
        "move.l  %%d0,%%d6\n\t"
        "move.l  %%d0,%%d7\n\t"
        "move.l  %%d0,%%a0\n\t"
        "move.l  %%d0,%%a1\n\t"
        "fmove.l %%d0,%%fp0\n\t"
        "fmove.x %%fp0,%%fp1\n\t"
        "fmove.x %%fp0,%%fp2\n\t"
        "fmove.x %%fp0,%%fp3\n\t"
        "fmove.x %%fp0,%%fp4\n\t"
        "fmove.x %%fp0,%%fp5\n\t"
        "fmove.x %%fp0,%%fp6\n\t"
        "fmove.x %%fp0,%%fp7\n\t"
        "fmove.l #0x30,%%fpcr\n\t"
        ::: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1",
            "fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7",
            "memory", "cc");
}

/*
 * kill(self, SIGUSR2) by a hand-written trap, with every register and
 * every FP register holding a known value. The handler, `clobber`,
 * wrecks them all; sigreturn must put back every one.
 */
/* Global, not static: the assembly below names them, and a static's
 * name is the compiler's to change. */
double fp_in[8] __attribute__((used)) = { 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5 };
double fp_out[8] __attribute__((used));
u32 fpcr_out __attribute__((used));

static s32 trap_kill_self(int pid)
{
    register u32 d0 __asm__("d0") = __NR_kill;
    register u32 d1 __asm__("d1") = (u32)pid;
    register u32 *out __asm__("a1") = after;

    __asm__ volatile (
        "movem.l %%d2-%%d7/%%a2-%%a6,-(%%sp)\n\t"
        "move.l  %%a1,-(%%sp)\n\t"
        "lea     fp_in,%%a0\n\t"
        "fmove.d (%%a0),%%fp0\n\t"
        "fmove.d 8(%%a0),%%fp1\n\t"
        "fmove.d 16(%%a0),%%fp2\n\t"
        "fmove.d 24(%%a0),%%fp3\n\t"
        "fmove.d 32(%%a0),%%fp4\n\t"
        "fmove.d 40(%%a0),%%fp5\n\t"
        "fmove.d 48(%%a0),%%fp6\n\t"
        "fmove.d 56(%%a0),%%fp7\n\t"
        "fmove.l #0x10,%%fpcr\n\t"
        "move.l  #12,%%d2\n\t"              /* SIGUSR2 */
        "move.l  #0x33333333,%%d3\n\t"
        "move.l  #0x44444444,%%d4\n\t"
        "move.l  #0x55555555,%%d5\n\t"
        "move.l  #0x66666666,%%d6\n\t"
        "move.l  #0x77777777,%%d7\n\t"
        "move.l  #0x10000000,%%a0\n\t"
        "move.l  #0x10000001,%%a1\n\t"
        "move.l  #0x10000002,%%a2\n\t"
        "move.l  #0x10000003,%%a3\n\t"
        "move.l  #0x10000004,%%a4\n\t"
        "move.l  #0x10000005,%%a5\n\t"
        "move.l  #0x10000006,%%a6\n\t"
        "trap    #0\n\t"
        "move.l  %%a1,-(%%sp)\n\t"
        "move.l  4(%%sp),%%a1\n\t"
        "movem.l %%d1-%%d7/%%a0,(%%a1)\n\t"
        "move.l  (%%sp)+,32(%%a1)\n\t"
        "movem.l %%a2-%%a6,36(%%a1)\n\t"
        "lea     fp_out,%%a0\n\t"
        "fmove.d %%fp0,(%%a0)\n\t"
        "fmove.d %%fp1,8(%%a0)\n\t"
        "fmove.d %%fp2,16(%%a0)\n\t"
        "fmove.d %%fp3,24(%%a0)\n\t"
        "fmove.d %%fp4,32(%%a0)\n\t"
        "fmove.d %%fp5,40(%%a0)\n\t"
        "fmove.d %%fp6,48(%%a0)\n\t"
        "fmove.d %%fp7,56(%%a0)\n\t"
        "fmove.l %%fpcr,fpcr_out\n\t"
        "fmove.l #0,%%fpcr\n\t"
        "addq.l  #4,%%sp\n\t"
        "movem.l (%%sp)+,%%d2-%%d7/%%a2-%%a6\n\t"
        : "+d"(d0), "+d"(d1), "+a"(out)
        :
        : "a0", "fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7",
          "memory", "cc");
    return (s32)d0;
}

static void test_registers_survive_a_handler(void)
{
    int pid = getpid(), i, ok = 1;
    s32 r;

    catch(SIGUSR2, clobber, 0, 0);
    count[SIGUSR2] = 0;
    r = trap_kill_self(pid);

    report("a handler that wrecks every register ran", count[SIGUSR2] == 1);
    report("  and kill's result was still in d0", r == 0);
    ok = after[0] == (u32)pid && after[1] == 12;
    for (i = 2; i < 14; i++) {
        static const u32 want[14] = {
            0, 0, 0x33333333, 0x44444444, 0x55555555, 0x66666666,
            0x77777777, 0x10000000, 0x10000001, 0x10000002, 0x10000003,
            0x10000004, 0x10000005, 0x10000006,
        };
        ok &= after[i] == want[i];
    }
    report("  and d1-d7, a0-a6 were all put back", ok);
    ok = 1;
    for (i = 0; i < 16; i++) {
        ok &= ((u32 *)fp_out)[i] == ((u32 *)fp_in)[i];
    }
    report("  and so were fp0-fp7", ok);
    report("  and the FP rounding mode", fpcr_out == 0x10);
    signal(SIGUSR2, SIG_DFL);
}

static void test_basics(void)
{
    struct sigaction old;
    sigset_t set, got;

    catch(SIGUSR1, counting, 0, 0);
    count[SIGUSR1] = 0;
    report("raise() runs the handler", raise(SIGUSR1) == 0 &&
           count[SIGUSR1] == 1);
    sigaction(SIGUSR1, 0, &old);
    report("sigaction reports the handler that is installed",
           old.sa_handler == counting);
    report("  with the restorer the library supplied",
           (old.sa_flags & SA_RESTORER) && old.sa_restorer != 0);

    /* The mask while a handler runs. */
    catch(SIGUSR1, nesting, 0, 0);
    count[SIGUSR1] = 0;
    max_depth = 0;
    raise(SIGUSR1);
    report("a signal raised in its own handler waits for it to finish",
           count[SIGUSR1] == 2 && max_depth == 1);
    catch(SIGUSR1, nesting, SA_NODEFER, 0);
    count[SIGUSR1] = 0;
    max_depth = 0;
    raise(SIGUSR1);
    report("  and with SA_NODEFER it nests",
           count[SIGUSR1] == 2 && max_depth == 2);

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    catch(SIGUSR1, usr1_then_usr2, 0, set);
    catch(SIGUSR2, usr2_note, 0, 0);
    order_n = 0;
    raise(SIGUSR1);
    report("sa_mask holds another signal until the handler returns",
           strcmp(order, "112") == 0);

    catch(SIGUSR1, counting, SA_RESETHAND, 0);
    count[SIGUSR1] = 0;
    raise(SIGUSR1);
    sigaction(SIGUSR1, 0, &old);
    report("SA_RESETHAND runs once and restores the default",
           count[SIGUSR1] == 1 && old.sa_handler == SIG_DFL);

    /* Blocking. */
    catch(SIGUSR1, counting, 0, 0);
    count[SIGUSR1] = 0;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, 0);
    raise(SIGUSR1);
    report("a blocked signal is held", count[SIGUSR1] == 0);
    sigpending(&got);
    report("  and sigpending shows it", sigismember(&got, SIGUSR1) == 1);
    sigprocmask(SIG_UNBLOCK, &set, 0);
    report("  and it arrives the moment it is unblocked",
           count[SIGUSR1] == 1);

    sigprocmask(SIG_BLOCK, &set, 0);
    raise(SIGUSR1);
    signal(SIGUSR1, SIG_IGN);
    sigpending(&got);
    report("ignoring a signal discards one already pending",
           sigismember(&got, SIGUSR1) == 0);
    sigprocmask(SIG_UNBLOCK, &set, 0);
    raise(SIGUSR1);
    report("  and an ignored signal does nothing", 1);

    /* What cannot be done. */
    report("SIGKILL cannot be caught",
           signal(SIGKILL, counting) == SIG_ERR);
    report("  nor SIGSTOP", signal(SIGSTOP, SIG_IGN) == SIG_ERR);
    sigfillset(&set);
    sigprocmask(SIG_SETMASK, &set, 0);
    sigprocmask(SIG_SETMASK, 0, &got);
    report("  and blocking everything does not block them",
           !sigismember(&got, SIGKILL) && !sigismember(&got, SIGSTOP) &&
           sigismember(&got, SIGTERM));
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, 0);
    {
        struct sigaction a;

        a.sa_handler = counting;
        a.sa_mask = 0;
        a.sa_flags = SA_SIGINFO;
        a.sa_restorer = 0;
        report("SA_SIGINFO is refused, not half supported",
               sigaction(SIGUSR1, &a, 0) == -EINVAL);
        a.sa_flags = 0;
        report("a handler with no restorer is refused",
               syscall(__NR_sigaction, SIGUSR1, (u32)&a, 0) == -EINVAL);
    }
    report("kill(pid, 0) finds a task that exists",
           kill(getpid(), 0) == 0);
    report("  and not one that does not", kill(9999, 0) == -ESRCH);
}

/* Start `sigtest poke <this pid> SIG MS` and return its pid. */
static int poke_me(int sig, u32 ms)
{
    static char a1[12], a2[12], a3[12];
    char *argv[5];

    utoa(a1, (u32)getpid());
    utoa(a2, (u32)sig);
    utoa(a3, ms);
    argv[0] = self;
    argv[1] = "poke";
    argv[2] = a1;
    argv[3] = a2;
    argv[4] = a3;
    return spawn(self, 5, argv, 0);
}

static void test_waiting(void)
{
    struct timespec ts;
    sigset_t set, got;
    int child, napper, st;
    u32 t0, took;
    s32 r;
    static char napms[] = "800";
    char *nargv[3];

    /* A second sleeper, which a signal to this task must not wake. */
    nargv[0] = self;
    nargv[1] = "nap";
    nargv[2] = napms;
    napper = spawn(self, 3, nargv, 0);

    catch(SIGUSR1, counting, 0, 0);
    count[SIGUSR1] = 0;
    child = poke_me(SIGUSR1, 200);
    r = pause();
    report("pause() returns EINTR after a handler runs",
           r == -EINTR && count[SIGUSR1] == 1);
    waitpid(child, &st, 0);

    /* Restarting. */
    catch(SIGUSR1, counting, 0, 0);
    child = poke_me(SIGUSR1, 200);
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    t0 = now_ms();
    r = nanosleep(&ts, 0);
    took = now_ms() - t0;
    report("without SA_RESTART an interrupted sleep returns EINTR",
           r == -EINTR && took < 700);
    waitpid(child, &st, 0);

    catch(SIGUSR1, counting, SA_RESTART, 0);
    count[SIGUSR1] = 0;
    child = poke_me(SIGUSR1, 200);
    t0 = now_ms();
    r = nanosleep(&ts, 0);
    took = now_ms() - t0;
    report("with SA_RESTART it is restarted after the handler",
           r == 0 && count[SIGUSR1] == 1 && took >= 1000);
    waitpid(child, &st, 0);

    /* sigsuspend: wait with a different mask, get the old one back. */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, 0);
    count[SIGUSR1] = 0;
    child = poke_me(SIGUSR1, 100);
    sigemptyset(&got);
    r = sigsuspend(&got);
    sigprocmask(SIG_SETMASK, 0, &got);
    report("sigsuspend waits with its own mask and returns EINTR",
           r == -EINTR && count[SIGUSR1] == 1);
    report("  and puts the old mask back", sigismember(&got, SIGUSR1) == 1);
    waitpid(child, &st, 0);
    sigprocmask(SIG_UNBLOCK, &set, 0);

    /* A child ending sends SIGCHLD, which a handler can catch. */
    catch(SIGCHLD, counting, 0, 0);
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, 0);
    count[SIGCHLD] = 0;
    child = poke_me(0, 10);         /* signal 0: just ends */
    sigemptyset(&got);
    sigsuspend(&got);
    report("a child ending sends SIGCHLD", count[SIGCHLD] == 1);
    waitpid(child, &st, 0);
    sigprocmask(SIG_UNBLOCK, &set, 0);
    signal(SIGCHLD, SIG_DFL);

    /* Nothing reaps an orphan yet, so every child is waited for. */
    waitpid(napper, &st, 0);
}

/* --- the modes --------------------------------------------------- */

static volatile int got_int;

static void on_int(int sig)
{
    got_int = sig;
}

static void privileged(void)
{
    u16 v;

    /* move from SR is privileged on every 68010 and later. */
    __asm__ volatile ("move.w %%sr,%0" : "=d"(v));
    puts("SUPERVISOR MODE REACHED\n");
    exit(1);
}

static int mode(int argc, char **argv)
{
    if (strcmp(argv[1], "poke") == 0 && argc == 5) {
        int pid = (int)atou(argv[2]), sig = (int)atou(argv[3]);

        msleep(atou(argv[4]));
        if (sig) {
            kill(pid, sig);
        }
        return 0;
    }
    if (strcmp(argv[1], "nap") == 0 && argc == 3) {
        u32 want = atou(argv[2]), t0 = now_ms(), took;
        struct timespec ts;

        ts.tv_sec = want / 1000;
        ts.tv_nsec = (want % 1000) * 1000000UL;
        nanosleep(&ts, 0);
        took = now_ms() - t0;
        puts(took + 10 >= want ? "sigtest: nap slept its full time\n"
                               : "sigtest: nap was CUT SHORT\n");
        return 0;
    }
    if (strcmp(argv[1], "catchint") == 0) {
        signal(SIGINT, on_int);
        puts("sigtest: waiting in pause for ctrl-C\n");
        pause();
        puts(got_int == SIGINT ? "sigtest: caught SIGINT in pause\n"
                               : "sigtest: pause returned without it\n");
        return 0;
    }
    if (strcmp(argv[1], "spincatch") == 0) {
        signal(SIGINT, on_int);
        puts("sigtest: computing until ctrl-C\n");
        while (!got_int) {
            ;                   /* no system calls in here at all */
        }
        puts("sigtest: caught SIGINT while computing\n");
        return 0;
    }
    if (strcmp(argv[1], "badstack") == 0) {
        catch(SIGUSR1, counting, 0, 0);
        puts("sigtest: taking a signal with an unusable stack\n");
        __asm__ volatile (
            "move.l  %0,%%d1\n\t"
            "moveq   #10,%%d2\n\t"
            "moveq   #37,%%d0\n\t"
            "move.l  #0x14000000,%%sp\n\t"  /* nothing is mapped here */
            "trap    #0\n\t"
            : : "g"(getpid()) : "d0", "d1", "d2", "memory");
        puts("SIGNAL DELIVERED ONTO NOTHING\n");
        return 1;
    }
    if (strcmp(argv[1], "forge") == 0) {
        static u32 frame[3 + sizeof(struct sigcontext) / 4 + 1];
        struct sigcontext *sc = (struct sigcontext *)&frame[3];
        u32 sp;

        __asm__ volatile ("move.l %%sp,%0" : "=d"(sp));
        memset(frame, 0, sizeof(frame));
        sc->sc_usp = sp - 256;
        sc->sc_pc = (u32)privileged;
        sc->sc_sr = 0x2700;         /* supervisor, interrupts masked */
        sc->sc_fpu[0] = 0x41000000;
        puts("sigtest: sigreturn to a context that asks for supervisor\n");
        __asm__ volatile (
            "move.l  %0,%%sp\n\t"
            "moveq   #119,%%d0\n\t"
            "trap    #0\n\t"
            : : "a"(frame) : "d0", "memory");
        return 1;
    }
    puts("sigtest: unknown mode\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        return mode(argc, argv);
    }
    test_gate();
    test_basics();
    test_registers_survive_a_handler();
    test_waiting();
    puts("sigtest: done\n");
    return 0;
}
