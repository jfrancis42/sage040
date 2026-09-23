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
| 38 | **Time zones**: `TZ`, `tzset`, `localtime` | `time.localtime`, `datetime`, and every timestamp a program prints | **done** |
| 39 | **terminfo**: a real database, not one terminal compiled in | curses reads it; so does `less` | **done** |
| 40 | **curses (ncurses)** | Python's `curses` and `_curses_panel`; `less` and any full-screen program | **done** |
| 30 | **The POSIX gaps**: FIFOs, `/dev/fd`, pseudo-terminals, `PATH_MAX`, `ARG_MAX` | the rest of what a port expects to find | pseudo-terminals and `PATH_MAX` **done**; FIFOs, `/dev/fd`, a listable `/dev` and `diff` open |
| 31 | **Python (CPython)** | the largest port yet | **done** -- 3.14.7 runs, 43 checks |
| 32 | **PATH**: that it is set, inherited, and searched | small, and everything assumes it | **done** |
| 37 | **cron** | needs the clock, a daemon, and somewhere to log | **done** |
| 43 | **`/var`, and `/var/log`**: the kernel's log to `/var/log/syslog` | asked for 2026-09-22 | **done** |
| 44 | **`/bin/less`** | asked for 2026-09-22; wants terminfo (39) | **done** |
| 45 | **libiconv** | CLISP needs it, and so does anything that converts between character sets; picolibc has `iconv` headers but no converters worth the name | |
| 46 | **gettext** | CLISP needs it; message catalogues, and the `_()` every GNU program is written around | |
| 47 | **readline** | CLISP needs it, and it is what makes any interactive program's line editing behave; over the terminfo of task 39. **CPython is rebuilt once this exists** -- its `readline` module is what gives the interactive interpreter a line editor, and it is switched off now for want of the library | |
| 48a | **The libraries CPython and CLISP want**, within reason (asked for 2026-09-22): **OpenSSL** (`ssl`, `hashlib`'s fast paths -- and what anything that speaks TLS needs), **SQLite** (`sqlite3`), **bzip2** and **xz** (`bz2`, `lzma`), **libffi** (`_ctypes`, and the only one that may not be reasonable: it wants a calling-convention trampoline written in m68k assembly, and whether libffi's m68k port still works is to find out) | |
| 48 | **CLISP**, a Common Lisp | asked for 2026-09-22. GNU CLISP is C plus a bytecode VM and wants its own build Lisp, as CPython wants a build Python; SBCL is out (it compiles to native code and has no m68k backend), ECL is the other candidate (it compiles to C). Depends on 45, 46 and 47 | |

**Not to be built until asked** (2026-09-22): tasks 45-48 -- libiconv,
gettext, readline, CLISP -- and 48a, the libraries CPython and CLISP
want. The list is here so the work is decided; the work itself waits.

| # | Task | Why here |
|---|------|----------|
| 50 | **libatomic**, GCC's own, built for this target | asked for 2026-09-22. **CPython does not need it any more**: the four 64-bit operations it wanted are implemented in the C library (`atomic64.c` in the m68k backend), over a table of locks, which is exactly what libatomic does on a target whose processor has no 64-bit atomic instruction -- and the 68040 has none. What building the real one would buy is a `-latomic` that EXISTS, for the configure scripts that test for it by linking against it, and the wider set libatomic carries (16-byte operations, the `__atomic_*_16` family) that nothing here has asked for. Small, and worth doing when a port asks for `-latomic` by name |
| 49 | **A native toolchain: gcc, gas, ld, gdb, objdump, nm, strip, ar, ranlib** -- the whole C and assembler chain running ON the machine, able to build the kernel and every program here without a cross compiler. Asked for 2026-09-22. Plus whatever they need to build and run: make (sbase has one), a shell (bash is here), binutils' and gcc's own dependencies -- GMP, MPFR, MPC, isl, zlib (here), libiconv and gettext (45, 46) | the point at which the machine stops needing another computer to exist. The 68040 is what gcc was written on; the question is memory and time, not capability -- gcc's own build wants a great deal of both, and 64 MB with swap is the constraint to measure first |

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

### 38. Time zones -- done

The clock keeps UTC and `TZ` says how to turn that into a local time;
picolibc's `tzset` reads it, and the shell sets `TZ=UTC0` by default so
a machine that has not been told where it is does not guess. There is no
zoneinfo database and no need of one: a POSIX TZ string carries its own
rules, which is what the format is for -- `export
TZ=MST7MDT,M3.2.0,M11.1.0` in `/etc/rc` is a machine in Colorado,
daylight saving included.

Nine checks in `libc/test/posixtest.c`, including a zone whose offset is
not a whole hour (IST-5:30) and the same zone read in January, which is
what tells a rule from a fixed offset.

The shell's own `date` still prints UTC and says so; local time is what
programs show, because that is where TZ lives.

---

### 39, 40. terminfo and curses -- done

ncurses 6.5 (`ports/ncurses`), built twice: a host build for `tic`,
which compiles the database, and the cross build for libncurses,
libtinfo, libform, libmenu, libpanel and the programs `tput`, `tset`,
`infocmp`, `clear` and `tabs`. The database is at
`/usr/share/terminfo`, seventeen terminals of it.

**Every terminal in it begins with a lower-case letter**, and that is
not an accident: terminfo stores one directory per first letter, FAT is
case-insensitive, and the full database has entries (`Eterm`, `emu`)
whose directories would collide. The build refuses a set with two names
that differ only in case.

Four terminals are compiled into the library as fallbacks (vt102, vt100,
dumb, unknown) so a program works on a disk with no database at all --
which is exactly why `kernel/curstest.sh` asks about wyse50 and xterm,
which are not. 28 checks, and the control renames the database away and
runs the whole thing again: vt102 keeps working, the other two fail.

---

### 31. Python -- done

CPython **3.14.7**, cross-built against picolibc, statically linked, with
every extension module built in (there is no dlopen here) and the
standard library on the disk at `/usr/local/lib/python3.14`. It runs:
big integers, floating point on the FPU, f-strings, comprehensions, the
standard library imported off a FAT filesystem, hashes, zlib, threads.

What it needed, in the order it was found -- and most of it was not
Python's fault:

- **`clone`, futexes and pthreads** (31a). CPython has required threads
  since 3.7.
- **Eight bugs in the C library**, each now a patch in `libc/patches/`:
  `SSIZE_MAX` defined with a cast, so unusable in `#if` (23); `struct
  rusage` with two fields instead of sixteen (24); `ioctl` declared
  taking `void *` rather than variadic, and refusing every request it
  did not know (25); `<sys/types.h>` not pulling in `<sys/select.h>` as
  glibc and the BSDs do (26); **`RLIMIT_*` numbers that were not
  Linux's**, so `getrlimit(RLIMIT_NOFILE)` asked the kernel about the
  resident set size and got a plausible wrong answer (27); and
  **`malloc` aligned to two bytes** (28).
- **`<sys/syscall.h>`**, generated from the same table `abicheck.sh`
  checks the kernel against, so the numbers a program calls and the
  numbers the kernel implements cannot drift apart.
- **64-bit atomics**, which the 68040 has no instruction for and this
  toolchain has no libatomic for: `atomic64.c` in the m68k backend,
  over a table of locks, as libatomic does it.
- **`fsync`, `fdatasync`, `tcdrain`, `tcsendbreak`**, declared by
  picolibc and implemented by nobody.

Two bugs were ours, and both had been waiting a long time:

- **`malloc` returned two-byte-aligned memory.** The m68k ABI aligns
  even a double to 2, so picolibc's malloc was conforming -- and
  CPython's garbage collector, which keeps flags in the low two bits of
  every object's list pointers, corrupted itself on its first
  collection. `python3 -c "print(1+1)"` died with a bus error at a wild
  address.
- **The static linker script never placed `.got`.** With no rule for it,
  ld puts the GOT where it likes and defines `_GLOBAL_OFFSET_TABLE_`
  from that, so code read its GOT entries past the end of the table, in
  .bss, where everything is zero. Every address fetched from the GOT
  came back null. Only a static program built `-fPIC` notices, and
  CPython is the first one here.

Two patches to CPython itself, both about the same thing: it asks the
COMPILER whether thread-local storage exists, and gcc offers `__thread`
on m68k whether or not the C library can support it (a TLS access
becomes a call to `__m68k_read_tp`, which needs an ELF TLS block per
thread). `ports/python/patches/` takes the thread-key path its own
comments describe, and one fixes a `struct timeval` declared in
prototype scope, which is a portability bug on any platform whose
headers do not drag `<sys/time.h>` in first.

Not done: `_ctypes` (no libffi, no dlopen), `ssl`, `sqlite3`, `bz2`,
`lzma`, `zstd`, `tkinter`. **`readline` waits for task 47**, and
CPython is to be rebuilt with it when that lands -- an interactive
interpreter with no line editing is the one obvious thing missing from
the port. `mimalloc` is off because it
wants thread-local storage; pymalloc is used instead.

---

### 32. PATH -- done

It was already there -- `spawn_on_path()` in `kernel/shell.c` walks
`$PATH` and has for some time, while a comment three hundred lines away
still said "there is one directory on this volume, so the name is the
path". What this task added is the comment's correction and the checks
that say what the rule is, in `kernel/apitest.sh`:

- the shell sets `PATH=/bin:.` before `/etc/rc` runs;
- a bare name is found along it, and the FIRST directory that has it
  wins -- the same program name in `/BIN` and `/OTHER` proves which;
- the current directory is searched LAST, so a program dropped in the
  working directory cannot quietly replace a system one;
- `./name` runs the one here whatever PATH says, because a name with a
  slash is a path and not a search;
- and a name on no directory of PATH is "not found", with the shell
  carrying on afterwards.

`execvp` in lib/ulib and picolibc's both do the same walk, so a program
that spawns by name agrees with the shell.

---

### 43. /var, /var/log, and the kernel's log -- done

The kernel keeps everything it prints in an 8 KB ring
(`kernel/klog.c`), readable as **`/dev/klog`**, and writes to no file
itself -- it cannot, because its first messages exist before there is a
disk driver and a panic has to work with the filesystem in any state.

**`klogd`** (`system/klogd.c`), started from `/etc/rc`, reads that
device -- which BLOCKS, so it is a sleeping task rather than a poll --
and appends each line to **`/var/log/syslog`** with a timestamp and the
machine's name. **`dmesg`** reads the same device for a machine with no
klogd running.

A read DRAINS the ring: what one reader takes, another does not see.
That is `/proc/kmsg`'s rule rather than `/dev/kmsg`'s, and it is the
right one here because the only reader is the one whose job is to put
the bytes somewhere they can be read repeatedly.

`/etc/rc` is installed by `make programs` if the disk has none, and
never overwritten if it has one: it is the machine's configuration.

`kernel/logtest.sh`: 10 checks, including the boot messages appearing in
a file read back with the HOST's tools, and a second boot with `/etc/rc`
renamed away -- where dmesg gets everything instead, which is both the
proof that the ring drains and the proof that `/etc/rc` is what starts
klogd.

---

### 37. cron -- done

sbase's `cron`, which reads `/etc/crontab`, keeps its pid in
`/var/run/crond.pid` and says what it is doing through `syslog(3)` --
which now appends to `/var/log/syslog`, the same file klogd writes the
kernel's messages to, so that a machine has ONE log rather than one per
source.

It is the first thing on this machine that happens because the CLOCK
said so rather than because somebody typed something, which makes
`kernel/crontest.sh` as much a test of the clock and of a long-lived
background task as of cron. It cannot be hurried: a minute-resolution
cron needs minutes, so the suite sets the machine's clock ten seconds
before a boundary and waits for two of them to pass -- one firing could
be an accident of startup, two cannot.

The control is a crontab line for a minute that will not come round
during the run. It must not fire, or the matching matches everything.

8 checks.

---

### 44. less -- done

GNU less 668 (`ports/less`), linked against **libtinfow** rather than
libncursesw: less asks terminfo what the terminal can do and writes the
sequences itself, and has no use for curses' windows.

It is the first program here that drives the terminal the way a
full-screen program does, so `kernel/lesstest.sh` is as much about the
terminal as about less. 12 checks: the first screenful, space for the
next, `G` for the end, `q` to leave -- with the escape sequences it used
to do it -- and then two cases that are not a terminal at all. Into a
pipe less is `cat`, and all 200 lines go through. On TERM=dumb it warns
that the terminal is not fully functional, waits to be told to carry on,
and then prints the file **without a single cursor-addressing
sequence**, which is the control: it asked the database rather than
assuming.

---

### 30. POSIX gaps

Found while porting awk, sed, grep, bash and sbase, and worth closing
whether or not anything needs them yet.

**Done since:** `PATH_MAX` agrees at 1024 (it was 256 in the kernel and
1024 in the headers, so a program could build a path the kernel would
refuse as too long); time zones (38); and **pseudo-terminals** --
/dev/ptmx, /dev/pts/N, `openpty`, `forkpty`, a line discipline with
canonical mode, erase and kill, ISIG, ECHO to the master, window size
and a foreground process group. 34 checks in `kernel/ptytest.sh`.

The pty work found three more C library bugs, each now a patch:
`fcntl(F_SETFL)` passed picolibc's O_NONBLOCK (0x4000) straight to a
kernel that means 0x800 by it, so a descriptor stayed blocking while
the program was certain it had not (31); `<stdlib.h>` declared `grantpt`
and none of the other three pty calls (29); and `<sys/ioctl.h>` had no
number for the two things a terminal is for beyond reading and writing
-- which process group is in the foreground, and which pts a master is
(30).

Still open:
- `execve` takes at most 256 arguments, which POSIX cannot express
  (ARG_MAX is bytes, 30,712 here); `xargs` builds lines by bytes.
- FIFOs: FAT cannot hold one; named pipes could live in the VFS.
- `/dev/fd`, which is what bash's process substitution wants.
- **`ls /dev` says "no such directory".** /dev is synthetic -- a name
  lookup, not a directory -- so a person cannot see what devices exist.
  A listable /dev is a VFS change, and it is the kind of thing somebody
  types on the first day.
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
