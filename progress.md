# Todo

What is **not** done. Everything that is finished is described as state:
[`os.md`](os.md) for the system, [`design.md`](design.md) for the machine,
[`README.md`](README.md) for how to build and run it, and
[`libc/README.md`](libc/README.md) and [`ports/`](ports/) for the C library
and the ported programs.

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk -- both can be increased and
have been.

A task is crossed off when its tests pass, and every task ships with them:
a check that could pass vacuously gets a negative control -- a deliberate
break that must make it fail.

**In hand now, in this order** (asked for 2026-09-22): **31** and what it
needs first, then 30, 32, 37, 38, 39, 40. The order below is the order of
work, not of numbering: Python is the target and everything above it in
this list is something Python wants.

| # | Task | Why here | State |
|---|------|----------|-------|
| 31a | **Threads**: `clone`, futexes, and a pthread layer | CPython has required threads since 3.7 -- this is the one real blocker | **done** |
| 38 | **Time zones**: `TZ`, `tzset`, `localtime` | `time.localtime`, `datetime`, and every timestamp a program prints | |
| 39 | **terminfo**: a real database, not one terminal compiled in | curses reads it; so does `less` | |
| 40 | **curses (ncurses)** | Python's `curses` and `_curses_panel`; `less` and any full-screen program | |
| 30 | **The POSIX gaps**: FIFOs, `/dev/fd`, pseudo-terminals, `PATH_MAX`, `ARG_MAX` | the rest of what a port expects to find | |
| 31 | **Python (CPython)** | the largest port yet | |
| 32 | **PATH**: that it is set, inherited, and searched | small, and everything assumes it | |
| 37 | **cron** | needs the clock, a daemon, and somewhere to log | |
| 43 | **`/var`, and `/var/log`**: the kernel's log to `/var/log/syslog` | asked for 2026-09-22; `syslog()` already writes to `/var/log/messages` | |
| 44 | **`/bin/less`** | asked for 2026-09-22; wants terminfo (39) | |

Left for later, and not started:

| # | Task | Why here |
|---|------|----------|
| 23 | Regression tests throughout | ongoing, never finished |
| 33 | A home directory, /home/jfrancis by default | HOME, ~, and where a shell starts |
| 34 | Users: a process belongs to one | ssh needs it; today everything is root |
| 35 | /etc/passwd: users, passwords, home directories, shells | getpwnam and friends read it |
| 36 | Multi-user for real: owners, groups and permissions | needs a filesystem that can hold them -- FAT cannot |
| 41 | ssh, client and server | after users, and needs real crypto |
| 42 | rsync | after ssh |

Tasks 1-22 built the system itself -- the address space, memory, signals,
pipes, subprocesses, sockets, the VT102 console, the C library, long file
names, `fsck`, shared libraries, paging and swap, interrupt-driven I/O --
and 24-29 are the standard tools: awk, sed, grep, bash and the sbase
utilities, each built from unmodified upstream source.

**Input is already interrupt-driven, and measured** (task 22, re-checked
2026-09-22): the serial port takes MFP channel 7 and the keyboard channel
1, each marking itself `tty_source_irq()` and draining through
`tty_input_irq()`; `kernel/devtest.sh` counts them (serial 19 then 39,
keyboard 24 for one typed command) and checks that nothing was lost and
that no interrupt arrived that nobody asked for. What is polled is a
source with no interrupt -- the console's own replies on `fbcon` -- and
resuming a drain that stopped because the ring was full. A stale comment
in `tty.c` said otherwise for a while; it does not now.

---

### 31a. Threads -- done

`clone(2)` with the thread set (CLONE_VM|FS|FILES|SIGHAND|THREAD),
futexes, and a POSIX thread layer over them in the C library. A thread
is a TASK that shares: one address space, one descriptor table, one
working directory, one set of signal handlers, one process id --
`gettid` is what tells threads apart. 52 checks in
`kernel/threadtest.sh`.

What it has: create, join, detach, exit, mutexes (normal, recursive,
errorcheck, timed), condition variables with `pthread_condattr_setclock`,
read/write locks, barriers, spinlocks, `pthread_once`, keys with
destructors, POSIX semaphores, `pthread_kill`, `pthread_sigmask`,
attributes including stack size and stack address.

What it does not, and why:

- **No `pthread_cancel`** (ENOSYS). Cancellation needs cancellation
  points throughout the C library; half of it is worse than none.
- **No compiler thread-local storage** -- `__thread` does not work, m68k
  has no thread register to spare, and CLONE_SETTLS is refused rather
  than accepted and ignored. `pthread_self()` finds a thread by the
  stack pointer it is standing on, and `errno` is a call
  (`__errno_location`, picolibc's `-Derrno-function`) into the thread's
  own descriptor.
- **No realtime scheduling attributes**: one policy, and `nice(2)` is
  how a program asks for less of the processor.

Four bugs it found, each now a check:

- **Task 0 had no descriptor table.** It is built by hand rather than by
  `alloc_task()`, so the fd-table refactor left `tty_init()` writing
  through a null pointer into the vector table -- which is mapped and
  writable. The machine booted, printed its banner and died at the first
  exception, exactly as the same mistake did once before.
- **picolibc's `_exit` called SYS_exit**, which ends one THREAD. A
  program that returned from main left its threads running with nothing
  to run for (`libc/patches/22`).
- **`pthread_kill` sent picolibc's signal numbers**, which are newlib's
  and not Linux's: SIGUSR1 is 16 here and 10 there, so the program was
  killed by SIGSTKFLT instead.
- **`errno` was a global**, shared by every thread.

---

### 30. POSIX gaps

Found while porting awk, sed, grep, bash and sbase, and worth closing
whether or not anything needs them yet.

- `PATH_MAX` is 256 in the kernel and 1024 in picolibc's headers.
- `execve` takes at most 256 arguments, which POSIX cannot express
  (ARG_MAX is bytes, 30,712 here); `xargs` builds lines by bytes.
- FIFOs: FAT cannot hold one; named pipes could live in the VFS.
- `/dev/fd` (bash's process substitution) and pseudo-terminals
  (`ptsname`, `openpty`; Python's pty module).
- Time zones: whether `localtime` honours `TZ` is unchecked.
- grep has no `-P` (no PCRE).
- No `diff` (POSIX): sbase has none. GNU diffutils (diff, cmp, diff3,
  sdiff) is the obvious port; bash's and sed's test suites use it.
- sed's and grep's own test suites, and sbase's, are shell scripts over a
  POSIX shell. bash is on the machine now, so they can be run on it rather
  than approximated by a harness on the host -- and awk's `space` and
  `system-status` tests, skipped for the same reason, with them.

### 31. Python

CPython, cross-built against picolibc. What it is known to lean on --
an assessment from CPython's configure and module list, not yet measured
here -- beyond what task 30 lists:

- `dlopen`/`dlsym` for extension modules (ld.so loads DT_NEEDED
  libraries; whether a program can load one at run time is to check),
  or every module linked in statically (`Setup.local`), which is how
  cross builds usually start.
- A build Python on the host of the same version, for the cross build.
- Threads: `_thread` needs pthreads, which there are none of. CPython
  can be configured without threads only in old versions; recent ones
  require them -- so either a pthread layer (one kernel thread per task
  as a start, or real threads via clone), or an older Python.
- `sigaltstack` (faulthandler), `getrusage`, `wait4`, `posix_spawn`,
  `sysconf` names, `/dev/urandom` or `getrandom` of cryptographic
  quality (`os.urandom`, hashing seeds), `select`/`poll`, `mmap`,
  termios, sockets, `getaddrinfo`, `localtime` with time zones.
- Memory: CPython's heap and a stdlib on disk -- the 512 MB disk is
  fine; 64 MB of RAM with swap should be.
- Floating point: `float` formatting and `repr` round-tripping lean on
  `strtod`/`printf` being exact; picolibc's are, but it is to test.

### 32-42. Users, terminals and the network

Not started, and in no particular order beyond what depends on what:

- **PATH (32)**: check that the variable exists, is inherited, and that a
  bare command name is looked up along it -- the shell sets it to
  `/bin:.` and nothing has tested the whole path.
- **A home directory (33)**: `/home/jfrancis` by default, `HOME` in the
  environment, `~` in the shell, and a login starting there.
- **Users (34)** and **/etc/passwd (35)**: a process belongs to a user;
  `getpwnam`, `getpwuid`, `getlogin`, `whoami` read a real file. Today
  every id is 0 and the kernel says so honestly.
- **Multi-user for real (36)**: owners, groups and permission bits, which
  FAT cannot hold -- so this is a filesystem as much as a kernel change,
  and the point at which `chown` and `chmod` stop being polite refusals.
- **cron (37)**: sbase has one, not built until there is a syslog daemon
  or a decision to log elsewhere.
- **Time zones (38)**: picolibc honours a POSIX `TZ` string; whether it
  does here is unchecked, and there is no zoneinfo database.
- **terminfo/termcap (39)** and **curses (40)**: libc/termcap has one
  terminal compiled in; a real database and a curses library are what
  full-screen programs expect.
- **ssh (41)** and **rsync (42)**: after users exist. ssh needs
  public-key crypto (the kernel has ChaCha20 and BLAKE2s, which is a
  start) and a pseudo-terminal, which is task 30's open list.
