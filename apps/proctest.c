/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * proctest - fork, execve, waitpid, and what a child inherits.
 *
 *   proctest                 the checks
 *   proctest exec-me X       what execve runs: report X and $PROCVAR,
 *                            exit 5
 *   proctest holdopen        sleep 300 ms holding whatever it was given
 *   proctest checksig        exit 0 if SIGUSR1 is default and SIGUSR2
 *                            still ignored after the exec
 */
#include "ulib.h"

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static u32 now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, 0);
    return (u32)tv.tv_sec * 1000 + (u32)tv.tv_usec / 1000;
}

static int shared_var = 1;

static void on_usr1(int sig)
{
    (void)sig;
}

static volatile int alarmed;

static void on_alarm(int sig)
{
    alarmed = sig;
}

static u32 procs(void)
{
    struct sysinfo si;

    sysinfo(&si);
    return si.procs;
}

static void test_fork(void)
{
    int pid, st, p[2], fd;
    char *heap;
    char c4[4];

    pid = fork();
    if (pid == 0) {
        /* The child: prove what it has, through its exit status. */
        int code = 0;

        if (shared_var != 1) {
            code |= 1;
        }
        shared_var = 2;
        exit(code);
    }
    report("fork returns the child's pid to the parent", pid > 0);
    report("  and the child ends as it should",
           waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
           WEXITSTATUS(st) == 0);
    report("  and its change to memory was its own", shared_var == 1);

    heap = malloc(10000);
    memset(heap, 'h', 10000);
    pid = fork();
    if (pid == 0) {
        exit(heap[0] == 'h' && heap[9999] == 'h' && getppid() > 1 ? 0 : 1);
    }
    report("the child has a copy of the heap",
           waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 0);
    free(heap);

    /* Descriptors are shared, and so is a file's position. */
    pipe(p);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "from-child", 10);
        exit(0);
    }
    close(p[1]);
    {
        static char b[16];
        s32 n = read(p[0], b, sizeof(b));

        report("a pipe made before fork joins parent and child",
               n == 10 && memcmp(b, "from-child", 10) == 0);
    }
    close(p[0]);
    waitpid(pid, &st, 0);

    fd = open("/PROCTEST", O_RDONLY);
    pid = fork();
    if (pid == 0) {
        read(fd, c4, 4);
        exit(0);
    }
    waitpid(pid, &st, 0);
    report("a descriptor's position is shared with the child",
           lseek(fd, 0, SEEK_CUR) == 4);
    close(fd);
}

static void test_wait(void)
{
    int pid, st, pid2;
    static char *sleepy[3] = { "/PIPETEST", "sleepy", 0 };

    pid = fork();
    if (pid == 0) {
        exit(7);
    }
    report("an exit status arrives as WEXITSTATUS",
           waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
           WEXITSTATUS(st) == 7 && !WIFSIGNALED(st));

    pid = spawn("/PIPETEST", 2, sleepy, 0);
    report("WNOHANG returns 0 while the child runs",
           waitpid(pid, &st, WNOHANG) == 0);
    kill(pid, SIGKILL);
    report("a killed child arrives as WTERMSIG",
           waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) &&
           WTERMSIG(st) == SIGKILL);

    pid = fork();
    if (pid == 0) {
        raise(SIGSTOP);
        exit(3);
    }
    report("WUNTRACED reports a child that stopped",
           waitpid(pid, &st, WUNTRACED) == pid && WIFSTOPPED(st) &&
           WSTOPSIG(st) == SIGSTOP);
    kill(pid, SIGCONT);
    report("WCONTINUED reports it continuing",
           waitpid(pid, &st, WCONTINUED) == pid && WIFCONTINUED(st));
    report("  and it then finishes normally",
           waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 3);

    pid = fork();
    if (pid == 0) {
        exit(1);
    }
    pid2 = fork();
    if (pid2 == 0) {
        exit(2);
    }
    {
        int a = waitpid(-1, &st, 0), b = waitpid(-1, &st, 0);

        report("waitpid(-1) collects any child, and each once",
               ((a == pid && b == pid2) || (a == pid2 && b == pid)));
    }
    report("with no children left, ECHILD", waitpid(-1, &st, 0) == -ECHILD);

    /* A signal interrupts a wait. */
    {
        struct sigaction act;

        act.sa_handler = on_alarm;
        act.sa_mask = 0;
        act.sa_flags = 0;           /* no SA_RESTART */
        act.sa_restorer = 0;
        sigaction(SIGALRM, &act, 0);
        pid = spawn("/PIPETEST", 2, sleepy, 0);
        alarm(1);
        report("a signal interrupts waitpid with EINTR",
               waitpid(pid, &st, 0) == -EINTR && alarmed == SIGALRM);
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        signal(SIGALRM, SIG_DFL);
    }
}

static void test_exec(void)
{
    int pid, st, p[2];
    static char *args[4] = { "/PROCTEST", "exec-me", "argument-one", 0 };
    static char *env[3] = { "PROCVAR=from-execve", "OTHER=x", 0 };
    static char *hold[3] = { "/PROCTEST", "holdopen", 0 };
    static char *chk[3] = { "/PROCTEST", "checksig", 0 };
    u32 t0;

    pid = fork();
    if (pid == 0) {
        execve("/PROCTEST", args, env);
        exit(99);                   /* only if execve failed */
    }
    report("execve runs the new program with its arguments and environment",
           waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 5);

    report("an execve that fails comes back with the error",
           execve("/NOSUCH", args, env) == -ENOENT);

    /* Close-on-exec: the exec'd child must not hold the write end. */
    pipe(p);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        execve("/PROCTEST", hold, 0);
        exit(99);
    }
    close(p[1]);
    t0 = now_ms();
    {
        char c;

        report("a close-on-exec write end is gone after execve",
               read(p[0], &c, 1) == 0 && now_ms() - t0 < 200);
    }
    close(p[0]);
    waitpid(pid, &st, 0);

    /* Caught signals go back to default; ignored ones stay ignored. */
    pid = fork();
    if (pid == 0) {
        signal(SIGUSR1, on_usr1);
        signal(SIGUSR2, SIG_IGN);
        execve("/PROCTEST", chk, 0);
        exit(99);
    }
    report("execve resets caught signals and keeps ignored ones",
           waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 0);
}

/* system(), the way a C library does it: fork, exec the shell, wait. */
static int run_sh(const char *cmd)
{
    static char *args[4];
    int pid, st;

    args[0] = "sh";
    args[1] = "-c";
    args[2] = (char *)cmd;
    args[3] = 0;
    pid = fork();
    if (pid == 0) {
        execvp("sh", args);
        exit(127);
    }
    if (waitpid(pid, &st, 0) != pid) {
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void test_shell(void)
{
    report("a program can run a command through /bin/sh",
           run_sh("exit 9") == 9);
    report("  including a pipeline",
           run_sh("pipetest out through-sh | pipetest count") == 0);
    report("  and a command that is not there is 127, as sh says",
           run_sh("nosuchprogram") == 127);
}

static void test_orphans(void)
{
    u32 before = procs();
    int pid, st;
    struct timespec ts;

    pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000;
            nanosleep(&ts, 0);
            exit(0);                /* an orphan by now */
        }
        exit(0);                    /* without waiting for it */
    }
    waitpid(pid, &st, 0);
    ts.tv_sec = 0;
    ts.tv_nsec = 400000000;
    nanosleep(&ts, 0);
    report("an orphan that finishes does not stay as a zombie",
           procs() == before);
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "exec-me") == 0) {
        const char *v = getenv("PROCVAR");

        puts("proctest: exec'd with ");
        puts(argv[2]);
        puts(" and PROCVAR=");
        puts(v ? v : "(none)");
        putch('\n');
        return strcmp(argv[2], "argument-one") == 0 && v &&
               strcmp(v, "from-execve") == 0 ? 5 : 6;
    }
    if (argc > 1 && strcmp(argv[1], "holdopen") == 0) {
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = 300000000;
        nanosleep(&ts, 0);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "checksig") == 0) {
        struct sigaction a, b;

        sigaction(SIGUSR1, 0, &a);
        sigaction(SIGUSR2, 0, &b);
        return a.sa_handler == SIG_DFL && b.sa_handler == SIG_IGN ? 0 : 1;
    }
    test_fork();
    test_wait();
    test_exec();
    test_shell();
    test_orphans();
    puts("proctest: done\n");
    return 0;
}
