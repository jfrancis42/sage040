/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ptracetest - ptrace(2), as a tracer sees it.
 *
 * Most tracees here are a fork of this program, which is the trick that
 * makes a tracer's checks exact: the child's variables and functions are
 * at the same addresses as the parent's, so the parent knows what it is
 * peeking at, poking, and planting a breakpoint in. Every check has an
 * answer the tracee could not have produced without the tracer having
 * done what it claims -- a getpid() that returns 12345, a write that
 * never happened, a function that stops when it is called.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __WALL
#define __WALL      0x40000000
#endif

#define NR_getpid   20
#define NR_write    4
#define PT_D0_OFF   (14 * 4)
#define PT_ORIG_OFF (16 * 4)
#define PT_PC_OFF   (18 * 4)

static int failures, checks;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    checks++;
    if (!ok) {
        failures++;
    }
}

volatile long shared_word = 0x11223344;
volatile int handled;

static void on_usr1(int s)
{
    (void)s;
    handled++;
}

/* Something to plant a breakpoint in. noinline, and it must do enough
 * that its first instruction is its own. */
__attribute__((noinline)) int target(int x)
{
    return x * 3 + 1;
}

static int stopped_with(int status, int sig)
{
    return WIFSTOPPED(status) && (WSTOPSIG(status) & 0x7f) == sig;
}

/* A child that asks to be traced and stops itself. */
static pid_t traced_child(void (*body)(void))
{
    pid_t pid = fork();

    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        body();
        _exit(0);
    }
    return pid;
}

/* --- the bodies the children run ----------------------------------- */

static void body_getpid(void)
{
    long p = getpid();          /* system call 20, every time */
    char line[64];
    int n = snprintf(line, sizeof(line), "child: getpid says %ld\n", p);

    write(1, line, (size_t)n);
    /* This write is the one the tracer skips: it must never appear. */
    write(1, "child: THIS WRITE WAS NOT SKIPPED\n", 34);
    _exit(p == 12345 ? 7 : 1);
}

static void body_word(void)
{
    _exit(shared_word == 0x55667788 ? 7 : 1);
}

static void body_breakpoint(void)
{
    _exit(target(4) == 13 ? 7 : 1);
}

static void body_signal(void)
{
    signal(SIGUSR1, on_usr1);
    kill(getpid(), SIGUSR1);        /* the tracer suppresses this one */
    kill(getpid(), SIGUSR1);        /* and passes this one on */
    _exit(handled == 1 ? 7 : 1);
}

static void body_fp(void)
{
    volatile double d = 1.5;

    __asm__ volatile ("fmove.d %0,%%fp2" : : "m"(d) : "fp2");
    raise(SIGSTOP);                 /* the tracer reads fp2 here */
    _exit(0);
}

static void body_fork(void)
{
    pid_t g = fork();

    if (g == 0) {
        _exit(9);
    }
    waitpid(g, 0, 0);
    _exit(7);
}

static void body_step(void)
{
    volatile int i, s = 0;

    for (i = 0; i < 100; i++) {
        s += i;
    }
    _exit(s == 4950 ? 7 : 1);
}

/* --- the checks --------------------------------------------------------- */

static void t_exec(void)
{
    pid_t pid = fork();
    int st = 0;

    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execl("/bin/true", "true", (char *)0);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    check("TRACEME then exec: the tracee stops with SIGTRAP", stopped_with(st, SIGTRAP));
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    check("  and CONT runs it to its exit", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void t_syscalls(void)
{
    pid_t pid = traced_child(body_getpid);
    int st = 0, stops = 0, saw_entry = 0, saw_regs = 0, sysgood = 1;
    int skipped = 0, writes = 0;
    long nr;
    struct user_regs_struct regs;

    waitpid(pid, &st, 0);
    check("a TRACEME child that raises SIGSTOP stops with it", stopped_with(st, SIGSTOP));
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD);
    ptrace(PTRACE_SYSCALL, pid, 0, 0);
    for (;;) {
        static int entry = 1;

        if (waitpid(pid, &st, 0) < 0 || !WIFSTOPPED(st)) {
            break;
        }
        stops++;
        if (WSTOPSIG(st) != (SIGTRAP | 0x80)) {
            sysgood = 0;
            ptrace(PTRACE_SYSCALL, pid, 0, (void *)(long)WSTOPSIG(st));
            continue;
        }
        errno = 0;
        nr = ptrace(PTRACE_PEEKUSER, pid, (void *)PT_ORIG_OFF, 0);
        if (entry && nr == NR_getpid) {
            saw_entry = 1;
            if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == 0 && regs.orig_d0 == NR_getpid &&
                regs.d0 == -38) {       /* Linux's ENOSYS: a raw register */
                saw_regs = 1;
            }
        }
        if (!entry && nr == NR_getpid) {
            ptrace(PTRACE_POKEUSER, pid, (void *)PT_D0_OFF, (void *)12345L);
        }
        if (entry && nr == NR_write) {
            writes++;
            if (writes == 2) {
                ptrace(PTRACE_POKEUSER, pid, (void *)PT_ORIG_OFF, (void *)-1L);
                ptrace(PTRACE_POKEUSER, pid, (void *)PT_D0_OFF, (void *)34L);
                skipped = 1;
            }
        }
        /* Entry and exit alternate -- a skipped call has its exit stop
         * too, straight away, as on Linux. */
        entry = !entry;
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
    }
    check("syscall stops, every one SIGTRAP|0x80 with TRACESYSGOOD", stops > 6 && sysgood);
    check("  getpid's entry: ORIG_D0 says 20, and GETREGS agrees, d0 -ENOSYS",
          saw_entry && saw_regs);
    check("  a result changed at the exit stop is what the program gets (12345),"
          " and a call skipped with ORIG_D0 = -1 never happens",
          skipped && WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void t_memory(void)
{
    pid_t pid = traced_child(body_word);
    int st = 0;
    long w;

    waitpid(pid, &st, 0);
    errno = 0;
    w = ptrace(PTRACE_PEEKDATA, pid, (void *)&shared_word, 0);
    check("PEEKDATA reads the child's variable", errno == 0 && w == 0x11223344);
    shared_word = 0;                /* only the PARENT's copy */
    check("POKEDATA writes it",
          ptrace(PTRACE_POKEDATA, pid, (void *)&shared_word, (void *)0x55667788L) == 0);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    check("  and the child sees the new value", WIFEXITED(st) && WEXITSTATUS(st) == 7);
    errno = 0;
    w = ptrace(PTRACE_PEEKDATA, 999999, (void *)&shared_word, 0);
    check("PEEKDATA of a process nobody traces is ESRCH", w == -1 && errno == ESRCH);
}

/*
 * /proc/PID/mem: the same memory as PEEK and POKE, a range at a time,
 * the offset being the address. What gdbserver uses and nothing else.
 */
static void t_procmem(void)
{
    char path[32];
    long w = 0, got = 0;
    unsigned short trap = 0x4e4f, orig = 0, back = 0;
    pid_t pid;
    int st = 0, fd;

    shared_word = 0x11223344;       /* t_memory zeroed the parent's copy */
    pid = traced_child(body_word);
    waitpid(pid, &st, 0);
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    fd = open(path, O_RDWR);
    check("/proc/PID/mem opens for reading and writing, by the tracer", fd >= 0);
    check("  pread at a variable's address reads the child's value",
          pread(fd, &w, 4, (off_t)(long)&shared_word) == 4 && w == 0x11223344);
    w = 0x55667788;
    check("  pwrite changes it, as PEEKDATA then sees",
          pwrite(fd, &w, 4, (off_t)(long)&shared_word) == 4 &&
          ptrace(PTRACE_PEEKDATA, pid, (void *)&shared_word, 0) == 0x55667788);
    errno = 0;
    check("  read-only text is written too, and read back as written",
          pread(fd, &orig, 2, (off_t)(long)target) == 2 &&
          pwrite(fd, &trap, 2, (off_t)(long)target) == 2 &&
          pread(fd, &back, 2, (off_t)(long)target) == 2 && back == 0x4e4f &&
          pwrite(fd, &orig, 2, (off_t)(long)target) == 2);
    errno = 0;
    check("  an unmapped address is EIO",
          pread(fd, &got, 4, (off_t)0x1000) == -1 && errno == EIO);
    close(fd);
    /* task/<tid>/: each thread's own directory, which is where gdb
     * opens mem -- task/<lwp>/mem, never the process's. */
    {
        DIR *d;
        struct dirent *e;
        int listed = 0, others = 0;
        struct stat sb;

        snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
        d = opendir(path);
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') {
                continue;
            }
            if (atoi(e->d_name) == (int)pid) {
                listed = 1;
            } else {
                others++;
            }
        }
        if (d) {
            closedir(d);
        }
        check("/proc/PID/task lists the one thread, by its tid", listed && !others);
        snprintf(path, sizeof(path), "/proc/%d/task/%d/mem", (int)pid, (int)pid);
        fd = open(path, O_RDONLY);
        w = 0;
        check("  task/TID/mem reads the same memory",
              fd >= 0 && pread(fd, &w, 4, (off_t)(long)&shared_word) == 4 &&
              w == 0x55667788);
        if (fd >= 0) {
            close(fd);
        }
        snprintf(path, sizeof(path), "/proc/%d/task/%d/task", (int)pid, (int)pid);
        check("  and a thread's directory has no task/ of its own",
              stat(path, &sb) == -1 && errno == ENOENT);
    }
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    check("  and the child ran on with what was written",
          WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void t_breakpoint(void)
{
    pid_t pid = traced_child(body_breakpoint);
    int st = 0, hit = 0, code_ok = 0;
    long orig, pc;
    siginfo_t si;

    waitpid(pid, &st, 0);
    errno = 0;
    orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)target, 0);
    /* trap #15 (0x4e4f) over the first instruction word: what gdb
     * plants on m68k. The page is read-only text. */
    check("POKETEXT plants a breakpoint in read-only code",
          errno == 0 && ptrace(PTRACE_POKETEXT, pid, (void *)target,
                               (void *)((orig & 0xffff) | 0x4e4f0000L)) == 0);
    check("  without touching the tracer's own copy of the page",
          (*(volatile unsigned short *)(void *)target) != 0x4e4f && target(1) == 4);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    if (stopped_with(st, SIGTRAP)) {
        errno = 0;
        pc = ptrace(PTRACE_PEEKUSER, pid, (void *)PT_PC_OFF, 0);
        hit = pc == (long)target + 2;
        code_ok = ptrace(PTRACE_GETSIGINFO, pid, 0, &si) == 0 &&
                  si.si_signo == SIGTRAP && si.si_code == 1;  /* TRAP_BRKPT */
    }
    check("the child stops at it: SIGTRAP, the pc just past the trap", hit);
    check("  and GETSIGINFO says TRAP_BRKPT", code_ok);
    /* Put the instruction back, back up the pc, and let it run on. */
    ptrace(PTRACE_POKETEXT, pid, (void *)target, (void *)orig);
    ptrace(PTRACE_POKEUSER, pid, (void *)PT_PC_OFF, (void *)(long)target);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    check("  restored, the function runs and returns the right answer",
          WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void t_step(void)
{
    pid_t pid = traced_child(body_step);
    int st = 0, steps = 0, moved = 1, traps = 1;
    long pc, last = -1;

    waitpid(pid, &st, 0);
    while (steps < 20) {
        if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0) < 0 || waitpid(pid, &st, 0) < 0) {
            break;
        }
        if (!stopped_with(st, SIGTRAP)) {
            traps = 0;
            break;
        }
        pc = ptrace(PTRACE_PEEKUSER, pid, (void *)PT_PC_OFF, 0);
        if (pc == last) {
            moved = 0;
        }
        last = pc;
        steps++;
    }
    check("SINGLESTEP: twenty steps, each a SIGTRAP at a new pc",
          steps == 20 && moved && traps);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    check("  and then it runs on to the right answer", WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void t_signals(void)
{
    pid_t pid = traced_child(body_signal);
    int st = 0, first = 0, second = 0;

    waitpid(pid, &st, 0);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    first = stopped_with(st, SIGUSR1);
    ptrace(PTRACE_CONT, pid, 0, 0);             /* suppressed */
    waitpid(pid, &st, 0);
    second = stopped_with(st, SIGUSR1);
    ptrace(PTRACE_CONT, pid, 0, (void *)SIGUSR1); /* delivered */
    waitpid(pid, &st, 0);
    check("a signal stops the tracee before it is acted on", first && second);
    check("  resumed with 0 it is never delivered, with the signal it is",
          WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void t_fpregs(void)
{
    pid_t pid = traced_child(body_fp);
    int st = 0;
    struct user_m68kfp_struct fp;

    waitpid(pid, &st, 0);                       /* the TRACEME stop */
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);                       /* its own SIGSTOP */
    memset(&fp, 0, sizeof(fp));
    /* fp2 is 1.5 in extended precision: sign and exponent 0x3fff, then
     * the mantissa 0xc0000000 00000000. */
    check("GETFPREGS reads fp2 as the 1.5 the child loaded",
          ptrace(PTRACE_GETFPREGS, pid, 0, &fp) == 0 &&
          (unsigned)fp.fpregs[6] == 0x3fff0000u && (unsigned)fp.fpregs[7] == 0xc0000000u &&
          fp.fpregs[8] == 0);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
}

static void t_attach(void)
{
    pid_t pid = fork();
    int st = 0;
    char path[64], buf[512];
    int fd, n;

    if (pid == 0) {
        for (;;) {
            usleep(50000);
        }
    }
    usleep(200000);
    check("ATTACH to a running child", ptrace(PTRACE_ATTACH, pid, 0, 0) == 0);
    waitpid(pid, &st, 0);
    check("  stops it with SIGSTOP", stopped_with(st, SIGSTOP));
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    fd = open(path, O_RDONLY);
    n = fd >= 0 ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) {
        close(fd);
    }
    buf[n > 0 ? n : 0] = 0;
    {
        char want[64];

        snprintf(want, sizeof(want), "TracerPid:\t%d\n", (int)getpid());
        check("/proc says t (tracing stop), and who the tracer is",
              strstr(buf, "State:\tt (tracing stop)") && strstr(buf, want));
    }
    check("DETACH lets it go", ptrace(PTRACE_DETACH, pid, 0, 0) == 0);
    usleep(200000);
    check("  and it runs, untraced: PEEKDATA is refused",
          ptrace(PTRACE_PEEKDATA, pid, (void *)&shared_word, 0) == -1 && errno == ESRCH);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    check("  and a SIGKILL ends it", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
}

static void t_fork(void)
{
    pid_t pid = traced_child(body_fork), grand = 0;
    int st = 0, event = 0, gst = 0;
    long msg = 0;

    waitpid(pid, &st, 0);
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACEFORK);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    if (WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | (PTRACE_EVENT_FORK << 8))) {
        event = 1;
        ptrace(PTRACE_GETEVENTMSG, pid, 0, &msg);
        grand = (pid_t)msg;
    }
    check("TRACEFORK: the fork is an event stop, the new pid its message",
          event && grand > 0);
    if (grand > 0) {
        waitpid(grand, &gst, __WALL);
        check("  and the new child is traced too, stopped with SIGSTOP",
              stopped_with(gst, SIGSTOP));
        ptrace(PTRACE_CONT, grand, 0, 0);
    }
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    check("  both run on to their ends", WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void t_kill(void)
{
    pid_t pid = traced_child(body_step);
    int st = 0;

    waitpid(pid, &st, 0);
    check("PTRACE_KILL", ptrace(PTRACE_KILL, pid, 0, 0) == 0);
    waitpid(pid, &st, 0);
    check("  ends a stopped tracee with SIGKILL", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    t_exec();
    t_syscalls();
    t_memory();
    t_procmem();
    t_breakpoint();
    t_step();
    t_signals();
    t_fpregs();
    t_attach();
    t_fork();
    t_kill();
    printf("ptracetest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
