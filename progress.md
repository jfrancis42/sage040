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

---

## STATE OF THINGS, 2026-09-23 (afternoon)

Everything that is broken, unfinished, unverified or waiting on a
decision, in one place, so that none of it has to be discovered by
reading the rest of this file. Dated, because it goes stale.

### Broken, or not working

**Nothing known.** `kernel/sshtest.sh` covers what was the last known
failure: ten successive ssh connections, a ping afterwards, a transmit
error count of zero, scp in both directions and rsync over ssh.

### Unverified -- believed working, not proven

| | |
|---|---|
| **A clean end-to-end regression** | No single run of `make test` has had every suite complete: `make` stops at the first failure, and the last run stopped part way. **Do this first**, and it matters more than it did: the disk became ext2 and every suite's staging changed with it. |
| **The suites not yet re-run on ext2** | `fstest`, `vmtest`, `edittest`, `usertest`, `threadtest`, `devtest`, `sotest`, `apitest`, `dftest` and `fscktest` have been run and fixed. The rest -- the ports' suites, the network suites, `pytest`, `nativetest` -- are converted but have not been run since. |
| **FAT16 is now untested** | The driver is still built and still registered, and `mount_root()` still falls back to it, but no suite exercises it any more: `fstest.sh` and `fscktest.sh` both test ext2 now. A small FAT mount-and-read suite is owed. |
| **`bashtest`'s `array` case** | Failed once, under heavy load, with the emulator killed mid-suite and the preceding test passing -- the signature of a harness timeout rather than a fault. Re-run on an idle machine to settle it. |
| **The suites' tolerance of load** | Most suites `sleep` a fixed time and assume the machine is ready. Under load it is not, and the failure looks like the thing being tested. `kernel/sshtest.sh` polls until the machine answers; the others do not. |

### Needs a decision -- yours, not mine

| | |
|---|---|
| **CLISP, or ECL** | CLISP's last release is 2.49, from **2010**, and does not build with a current gcc even on an ordinary Linux machine. It is a porting project rather than one more port. ECL is actively maintained and compiles Lisp to C -- which this machine can now compile. The bootstrap problem that made either hard is solved (see task 48). |
| **Permission enforcement** | The filesystem is ext2 now, so a file HAS an owner, a group and a mode, and a file a user creates belongs to that user. Nothing checks any of it: no open, no unlink, no directory search consults a mode bit. Storing them right was the prerequisite and is done; the enforcement itself touches every system call and is a task of its own. |
| **Symlinks** | ext2 holds them and `stat` reports `S_IFLNK` rather than mistaking one for a short file, but nothing creates or follows one. That needs `symlink`, `readlink`, `O_NOFOLLOW` and following during a path walk -- VFS and system-call work, not filesystem work. |

### Unfinished, and nothing is blocking them

- **Task 30's remainder**: FIFOs, `/dev/fd`, a listable `/dev`, and
  `diff`. A FIFO still has to live in the VFS -- ext2 could hold one,
  but nothing creates a device node or a FIFO on disk yet. Between them
  FIFOs and `/dev/fd` are what bash's `<(...)` needs.
- **gdb**, native. It is C++ and libstdc++ now exists, so the
  remaining obstacle is `ptrace` in the kernel -- which does not exist
  at all. Without it a debugger cannot stop, inspect or step anything.
- **libatomic** (50). Nothing has asked for it; the reasoning is kept
  below.
- **CLISP or ECL** (48), pending the decision above.

### Settled since the last revision of this section

- **The gcc integer-type deviation is fixed at the root.**
  `ports/gcc/patches/02-m68k-linux-integer-types.patch` makes
  `m68k-elf` take its integer types from gcc's `glibc-stdint.h`, as
  `m68k-linux` does, and sets `SIZE_TYPE`/`PTRDIFF_TYPE` to match. So
  `uint32_t` is `unsigned int`, `uint32_t *` is compatible with
  `unsigned *`, `size_t` is `unsigned int` and `PRIu32` is `"u"`. The
  whole class of incompatible-pointer build failures (zstd, OpenSSL,
  bfd, libsframe) is gone, and it is ABI-neutral: `int` and `long` are
  both 32 bits here with the same alignment and passing. The cross
  compiler, picolibc and the native toolchain (binutils, gmp, mpfr,
  mpc, libstdc++, gcc) are all rebuilt against it.

  Two things it makes possible and nobody has done yet: the per-port
  workaround patches (`ports/binutils/patches/01`, `02`,
  `ports/openssl/patches/01`) can come out one at a time, each with a
  rebuild to confirm; and `ZSTD_LEGACY_SUPPORT` in
  `ports/zstd/build.sh` was set to 0 only because of this, and can go
  back to 1. Neither is urgent -- they are working as they are.
- **The filesystem is ext2** (see `design.md` §8). The machine boots
  from it, the boot ROM reads it, `e2fsck` checks what the kernel
  wrote, and `tools/fsimg.sh` replaced every mtools call in the
  Makefiles and the suites.

### Known limitations, accepted rather than outstanding

These are properties of the machine, written up in `design.md` and
`os.md`. They are here so they are not mistaken for bugs.

- **No `dlopen`**, so no `ctypes` -- and that is the loader's missing
  feature, not libffi's. libffi is built and demonstrably works.
- **No thread-local storage.** The 68040 has no thread pointer
  register; `__thread` needs `PT_TLS` in `ld.so`, a per-thread block,
  and `__m68k_read_tp`, all three.
- **No `crypt(3)`**, so no ssh password authentication. Public keys
  work.
- **Users protect nothing** -- the disk records owners and modes now;
  nothing enforces them. See "Needs a decision" above.
- **Object files built on the machine are not byte-reproducible.** The
  native assembler leaves uninitialised bytes in section padding where
  the cross one leaves zeroes. Every section a tool reads is identical.
- **`xz` at its default preset will not run**: `-6` wants about 94 MB
  and the machine has 64. `-1` works.

### Where the traps are written down

Facts about this machine that cost time to learn live where somebody
will meet them again, not in a list here: properties of the hardware
in `design.md`, properties of the system in `os.md`, and the reason a
particular line of code is the way it is in the comment above that
line.

---

**In hand now** (asked for 2026-09-22, evening): the whole remaining
list, in this order -- **49** (the native toolchain) first, then
**CPython rebuilt** against the new libraries, then `df`/`du`, then
home directories, users and `/etc/passwd` (33, 34, 35), then ssh with
scp and rsync (41, 42), then the remaining POSIX gaps (30), then
libiconv/gettext/libatomic (45, 46, 50), and **CLISP last** (48).

**Task 36 was deferred here and its first half has since been done**:
the filesystem is ext2 (task 52), so a file now HAS an owner, a group
and a mode, and one a user creates belongs to that user. What is still
open is ENFORCEMENT -- no open, no unlink and no directory search
consults a mode bit yet.

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
| 51 | **`df` and `du`** | asked for 2026-09-22 | **done** -- `du` is sbase's (`-k -h -a -s -d -x`); `df` is new, with `-h`, `-k` and `-i`. 14 checks, numbers verified against the host's own tools |
| 49 | **A native toolchain** | asked for 2026-09-22 | **done** -- binutils AND gcc 15.2.0 run on the machine; 15 checks, and the code it generates is identical to the cross compiler's |
| 41 | **ssh, client and server, with scp** | asked for 2026-09-22 | **mostly done** -- Dropbear 2026.94. A real OpenSSH client authenticates into the machine by public key and runs commands. `scp` builds and does not work; see below |
| 42 | **rsync** | asked for 2026-09-22 | **done** -- 3.4.1, over ssh, verified both locally and from another machine |
| 33 | **A home directory** | | **done** with 34/35 |
| 34 | **Users: a process belongs to one** | | **done** -- identity only; enforcement needs 36 |
| 35 | **/etc/passwd** | | **done** |
| 45 | **libiconv** | character-set conversion; CLISP requires it | **done** -- 1.18 |
| 46 | **gettext** | message catalogues, and the `_()` every GNU program is written around | **done** -- 0.23.1, the runtime only |
| 47 | **readline** | line editing in any interactive program, over the terminfo of task 39 | **done** -- 8.3, and CPython is rebuilt against it |
| 48a | **The libraries CPython wants** | asked for 2026-09-22 | **done**, and **CPython is rebuilt against them**: 90 built-in modules where there were 87 |
| 48 | **CLISP**, a Common Lisp | asked for 2026-09-22 | **not started, deliberately** -- the bootstrap is solved but the release is from 2010 and does not build with a current compiler. See below; ECL is the likely answer |
| 50 | **libatomic** | for a configure script that tests for `-latomic` by name | not done; nothing has asked for it |
| 52 | **ext2**, in place of FAT16 | permissions, ownership, real names, real inode numbers -- everything task 36 needs, and a filesystem the host can CHECK | **done** -- `kernel/fs/ext2.c`, the boot ROM reads it, `tools/fsimg.sh` replaced every mtools call, `fstest.sh` is 63 checks ending in `e2fsck` |

**What is left**, and none of it is blocked on anything:

- **task 30's remainder**: FIFOs, `/dev/fd`, a listable `/dev`, and
  `diff`. FIFOs and `/dev/fd` are kernel work -- a FIFO has to live in
  the VFS, since a FAT directory entry cannot hold one -- and between
  them they are what bash's `<(...)` needs.
- **task 36**, the filesystem that can hold an owner, which is what
  turns the users of 33-35 into enforcement. Deferred on purpose.
- **task 48**, a Lisp. See below.
- **task 50**, libatomic, which nothing has asked for.
- **`scp`**, which builds and does not run.
- **`dlopen`** and **thread-local storage**, both written up in
  `design.md` as decisions about the machine rather than items on a
  list.

**On libatomic (50), since the reasoning is worth keeping:** CPython
does not need it. The four 64-bit operations it wanted are implemented
in the C library (`atomic64.c` in the m68k backend) over a table of
locks, which is exactly what libatomic does on a processor with no
64-bit atomic instruction -- and the 68040 has none. What the real one
would buy is a `-latomic` that EXISTS, for configure scripts that test
for it by linking, and the 16-byte operations nothing here has asked
for. Small, and worth doing when a port asks for it by name.

Left for later, and not started:

| # | Task | Why here |
|---|------|----------|
| 23 | Regression tests throughout | ongoing, never finished |
| 36 | Multi-user for real: owners, groups and permissions | the filesystem can hold them now (52), and does: a file has a uid, a gid and a mode, and a file a user creates belongs to that user. What is left is the ENFORCEMENT -- every system call that opens, unlinks or walks a path has to consult one |

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

### 51. `df` and `du` -- done

`du` was already there: sbase's, with `-a -s -d depth -h -k -H -L -P
-x`. `df` is new -- `system/df.c`, installed as `/bin/df` -- with
`-h`, `-k` and `-i`. `kernel/dftest.sh`, 14 checks.

**The numbers are checked against the host's**, not against
themselves. `fsimg df` reports the free space on the same image before the
machine boots, and the two agreed to the kilobyte.
The file sizes are deliberately awkward -- 300,000 and 70,000 bytes,
which are a whole number of neither kilobytes nor blocks -- so a
`du` that rounded the wrong way could not come out right by accident.

**The negative control** is the last check: `/bin/df` is moved aside
and `df` run again. The shell has a `df` built in, so something must
still answer -- and its heading must be the BUILT-IN's. Without that,
every check above would pass just as well on a machine where the
program was never run at all.

Two bugs on the way, and one of them was waiting rather than new:

- **`struct statfs` was this system's own layout behind Linux's system
  call number**, and one of its fields was a `const char *` pointing
  at the string `"fat16"` **in the kernel**. The kernel's own shell
  could print it; a program could not, the kernel being mapped
  supervisor-only, so `/bin/sh`'s `df` had an access fault waiting in
  it. `statfs` is Linux's layout now, `f_type` is
  `MSDOS_SUPER_MAGIC`, and the volume label -- which Linux's `statfs`
  has no field for -- moved to `fsctl(FSCTL_LABEL)`.

- **The shell's built-in `df` shadowed the program**, so `df -h`
  silently ignored the `-h`. Built-ins normally win, which is right
  for `echo`; it is wrong for a built-in that takes no arguments and
  prints one fixed report. The built-in now runs `/bin/df` when it
  exists and only answers itself when it does not -- which is what
  keeps it useful on a disk whose filesystem is the thing being
  investigated.

- **ulib programs never linked `libgcc`.** Nothing had needed it,
  which is not the same as nothing ever needing it: `df` multiplies a
  cluster count by a cluster size and must work in 64 bits to survive
  a 4 GB volume, and `__udivdi3` lives in libgcc. `lib/program.mk`
  links it now.

### 49. A native toolchain -- DONE

**binutils 2.45 runs on the machine.** as, ld, ar, ranlib, nm,
objdump, objcopy, strip, readelf, size, strings, addr2line, c++filt,
elfedit, gprof -- about 15 MB, installed to `/usr/bin`, which is now
on the shell's PATH. Verified by running them: a `.s` file assembled
by the machine's own `as`, linked by its own `ld`, and inspected with
its own `nm`, `size`, `readelf` and `strip`.

It is a **Canadian cross** -- `--build` is this workstation, `--host`
and `--target` are the Sage040 -- built entirely on mother. Nothing is
compiled inside the emulator; the emulator runs the result, which is
the test and not the build.

Three things it needed:

1. **`libc/sage040.specs`.** Every program here used to be linked with
   eleven explicit flags, which works while a Makefile in this tree
   does the linking and stops working the moment anything else does.
   binutils' own top-level configure checks the compiler with
   `${CC} -o conftest ${CFLAGS} ${CPPFLAGS} ${LDFLAGS} conftest.c` and
   no `${LIBS}` anywhere. The knowledge belongs in the compiler: `gcc
   hello.c -o hello` now links a dynamic program against
   `/lib/libc.so`, and `gcc -static` a static one laid out by
   `sage040.ld`. This is also exactly what the native compiler needs,
   since somebody at a prompt will type no more than that.

2. **Every flag baked into `$CC`.** binutils configures a dozen
   subdirectories and passes CFLAGS down to each but not CPPFLAGS --
   so libiberty could not find `<stdio.h>`, concluded the compiler
   could not link at all, and failed every later test with "Link tests
   are not allowed after GCC_NO_EXECUTABLES". Moving the flags to
   CFLAGS fixed the compile tests and not the preprocessor-only ones,
   which autoconf runs as `$CPP $CPPFLAGS` with no CFLAGS near them:
   `AC_HEADER_STDC` came back "no", and libiberty then built regex.c
   with no `<stdlib.h>` and failed on "too many arguments to function
   'malloc'". Flags inside CC are in all three.

3. **`ac_cv_tls=none`, exported for the whole build.** binutils tests
   for thread-local storage by COMPILING `static thread_local int
   bar;` and never linking it. Compiling succeeds -- the m68k back end
   emits a call to `__m68k_read_tp`, which is how a CPU with no thread
   pointer register does TLS -- and the link then fails on that
   symbol. bfd uses TLS for one variable and falls back to a plain
   global without it. The variable has to be EXPORTED rather than
   named on the configure line, because bfd's own configure is run by
   `make`, later, with a cache file of its own.

**GMP, MPFR and MPC are built for the machine**, as their own ports
rather than from gcc's in-tree copies -- gcc needs all three to fold
constant expressions exactly, and CLISP's bignums will want the same
libgmp. GMP needs `--disable-assembly` (its m68k assembly is selected
by a host table this build does not match) and `-std=gnu17` (one of
its own configure probes declares `void g(){}` and calls it with six
arguments, which C23 refuses).

**The C library is installed on the machine**, at `/usr/include` and
`/usr/lib`, with `crt0.o`, `crt0-dyn.o`, `sage040.ld` and `libgcc.a`
beside it. Nothing needed that while every program was cross-compiled.

**The chain is longer than it looks, and all but the last link is
done.** GCC 15 is written in C++, so a compiler that runs on this
machine needs a C++ standard library that runs on this machine -- and
the existing cross toolchain is C-only, with no cross `g++` at all. So:

1. **a cross g++**, gcc 15.2.0 with `c,c++`, into
   `~/m68k/install-cxx` -- a prefix of its own, so the C compiler
   everything else depends on is never at risk. **Done.**
2. **libstdc++ for the target**, `ports/libstdcxx`, 1.5 MB. **Done.**
3. **the native gcc**, `ports/gcc`. **Done.** 79 MB stripped, of
   which cc1 is 28 MB; unstripped it is 1.1 GB, which does not go on
   a 512 MB disk and is of no use on a machine with no debugger.
4. gdb, which is also C++ and additionally wants `ptrace` in the
   kernel. Not started.

**What the Canadian cross needed, beyond the three triplets** -- each
of these failed in a way that named something else:

- **`--without-isl`**: isl is configured before gmp is anywhere it can
  find it, and says "gmp.h header not found". Nothing here needs
  polyhedral loop transformation.
- **`ports/gcc/patches/01`, dropping the bundled gettext.** gcc 15
  carries gettext in its tree and configures it whatever
  `--disable-nls` says. Its gnulib decides `uselocale()` is usable and
  then calls `uselocale(NULL)` where this `locale_t` is not a pointer,
  and reaches for C23's `ckd_add`. Each stopped the build in a library
  that nothing in this configuration will ever call.
- **tools under the target's own name.** `--target=m68k-unknown-elf`
  makes the build look for `m68k-unknown-elf-gcc` when it wants to
  compile something for the target; the cross tools are installed as
  `m68k-elf-*`. Without a directory of symlinks it linked `xgcc` and
  then died on "command not found". The gcc one must point at the
  **C++-capable** cross compiler, because the build runs it on gcc's
  own C++ self-test and the C-only one answers "language c++ not
  recognized".
- **`all-host`, not `all`.** `all` goes on to build the target
  libgcc, which means running the compiler just built -- an m68k
  program, on this workstation. There is nothing to build there
  anyway: libgcc for this target already exists, same version, same
  source.
- **`extern "C"` in every network header.** This one was a real bug
  rather than a build-system quirk: nothing in this tree had ever been
  written in C++, so `libc/net`'s headers had no guards and a C++
  translation unit gave `getaddrinfo` and friends mangled names. gcc's
  own `c++tools` is C++ and found it immediately.

**`kernel/nativetest.sh`: 15 checks, all passing.** The machine runs
`gcc`, compiles `hello.c`, links it with its own `ld`, and runs the
result; compiles a program whose 64-bit arithmetic, floating point and
type sizes are all checked against known answers; and compiles two
source files separately and links them together.

**And the code it generates is the cross compiler's code.** The same
source is compiled here and there, and `.text`, `.rodata` and `.data`,
the disassembly instruction for instruction, the symbol table and the
relocations are all identical. So this is not merely a compiler that
runs on the machine -- it is the same compiler, which is what rules
out a native gcc that is subtly miscompiled and quietly produces wrong
code.

**One genuine difference, worth knowing:** the raw object files are
NOT byte-identical. Two bytes differ, both alignment padding -- before
the section header table and before `.comment` -- where the native
assembler leaves `0x1b` and `0x04` and the cross one leaves zeroes. So
one of them writes whatever was in the buffer. Nothing reads those
bytes and every section a tool looks at agrees, but it does mean
object files built on this machine are not reproducible byte for byte.

**It needs a bigger machine than the default**: `NATIVE_RAM_MB=256`
and a 512 MB disk. cc1 is 28 MB of program before it allocates
anything.

### 45, 46. libiconv and gettext -- done

libiconv 1.18 and gettext 0.23.1's runtime, both
**signature-checked against GNU's keyring** (both are Bruno Haible's,
which is a pleasant coincidence given he also maintains CLISP). Both
built first try.

gettext is deliberately **the runtime only** -- libintl and
`gettext(1)`. The tools (xgettext, msgfmt, msgmerge) are a developer's
rather than a machine's: catalogues are compiled on a workstation and
the `.mo` files copied over. Building them would mean porting their
gnulib, which has been the most troublesome thing in this tree by a
distance -- gcc's bundled copy had to be switched off entirely
(`ports/gcc/patches/01`).

### 48. CLISP -- not started, deliberately, and here is what it needs

Its dependencies are now all built: libiconv, gettext's runtime,
readline, and GMP (which CLISP uses for bignums and which gcc needed
anyway). Nothing is missing on that side.

**It was not started because it cannot be done well in what was left
of the night, and half a Lisp is worse than none.** The obstacle is
not the dependencies, it is the bootstrap, and it is worth writing
down while it is clear:

CLISP's build compiles a C program, `lisp.run`, and then **runs it**
to compile CLISP's own Lisp sources into the memory image
`lispinit.mem` that the finished system needs. A cross build therefore
has to execute a target binary partway through. That is the whole
problem, and there are three ways at it:

1. **Run `lisp.run` under `qemu-m68k` user-mode emulation on the
   workstation.** **This has now been tried and it works** --
   `libc/crt0-qemu.s` and `libc/test/qemutest.sh`, 8 checks. A
   statically linked Sage040 program runs on this workstation under
   `qemu-m68k` with no Sage040 and no kernel of ours involved: it
   opens, writes, reads and stats files on the host's filesystem, gets
   `ENOENT` as 2, allocates from the heap, and returns the right
   answers from `getpid` and `time`.

   The only thing it needed was a different `crt0`. This kernel enters
   a program with `jsr`, so argc is at `4(%sp)`; Linux puts argc at
   `0(%sp)` with the argv **array** inline above it rather than a
   pointer to it. Reading it the kernel's way dereferenced the first
   four characters of the program name -- `si_addr=0x2f746d70`,
   `"/tmp"`.

   That makes route 1 the way to build CLISP, and it is worth more
   than CLISP: it is the **strongest evidence available that the ABI
   claim is true**. Every other check of it in this tree is made by
   something in this tree; `qemu-m68k` is somebody else's
   implementation of Linux/m68k written with no knowledge of this
   project. And it runs a program for the machine in seconds rather
   than a minute of booting.
2. **Run it on the machine**, under the full emulator, with the build
   driven over the serial console. Certain to work and slow, and the
   build would have to be split around the handover.
3. **Use a host CLISP to produce the image.** The usual answer for
   cross-building CLISP, and the memory image is architecture- and
   word-size-specific, so a host image is not usable directly.

Route 1 first, then 2. Neither is a night's work to do carefully.

**AND THERE IS A BIGGER PROBLEM THAN THE BOOTSTRAP, which is worth
knowing before any time goes into it.** GNU CLISP's last release is
**2.49, dated 2010** -- checked against ftp.gnu.org, where 2.49 is
still the newest directory. It is a fifteen-year-old C codebase, and
fifteen years is exactly the span over which C compilers stopped
tolerating what it does: implicit declarations, aliasing assumptions,
and the pre-C23 spellings that have already bitten GMP (`void g(){}`
called with six arguments) and zstd in this tree. It does not build
with a current gcc on an ordinary Linux machine without patches, let
alone against picolibc on m68k.

So CLISP is not "one more port". It is a porting project of its own,
and the bootstrap -- now solved -- was the easy half.

**ECL is the alternative and is probably the better answer.** It was
named in this list from the start as the other candidate, it is
actively maintained, and it compiles Lisp to C and hands it to the
system compiler -- which this machine now has. That last point changed
tonight: an implementation that needs a C compiler at run time was
impossible here this morning and is not now.

The choice between them is the user's, and it is a real choice rather
than a formality: CLISP is what was asked for, ECL is what is likely
to work.

`m68k-elf` takes its integer types from gcc's `newlib-stdint.h`, where
`uint32_t` is **`long unsigned int`**. Linux/m68k takes them from
`glibc-stdint.h`, where it is **`unsigned int`**. This system's ABI is
Linux/m68k's on purpose, so that is a real deviation, and it has now
broken four builds: zstd's legacy decoders, OpenSSL's QUIC assist
thread, bfd's target jump tables and libsframe. Each needed a patch
saying the same thing.

Two ways out:

- **Rebuild the cross gcc** so this target describes itself the way
  Linux/m68k does. It kills the whole class permanently, and it is
  ABI-safe: `int` and `long` are both 32 bits here with the same
  alignment and the same passing convention, so no existing binary
  changes and nothing needs recompiling to stay compatible.
- **Patch each package as it trips.** Each patch is small, correct and
  upstreamable.

The second was taken, deliberately, because the first means rebuilding
the toolchain everything else depends on, unattended, overnight. The
first is probably the right long-term answer and would help CLISP,
which is fussy about exactly this.

### 33, 34, 35. Users, /etc/passwd and home directories -- done

`kernel/usertest.sh`, 16 checks. A task carries a real, effective and
saved user and group id, inherited across fork and exec; `setuid`,
`setgid`, `setreuid`, `setregid`, `setresuid` and `setresgid` move
them under POSIX's rules, and a change reaches **every thread of the
process**, because credentials belong to a process and a thread left
holding the old one would be a hole rather than a feature.

`/etc/passwd` and `/etc/group` are installed when missing and left
alone when present -- unlike `/etc/rc`, which has an unedited default
to recognise. A passwd file is a list of people, and replacing one
because it resembled the shipped version would delete a user.

The shell expands `~` and `~user`, reading `/etc/passwd` itself with
`open()` and `read()` because it may not call `getpwnam` -- it is not
linked against the C library. That leaves two independent
implementations of the same lookup, so the suite checks that they
agree: the shell's parser against the C library's `getpwuid`, through
sbase's `whoami`.

**The last check in the suite reads root's file as an ordinary user's
would and PASSES, on purpose.** There is no file ownership on a FAT
volume, so nothing is protected by any of this. A suite that omitted
that check could be read as evidence of a protection that does not
exist.

### 41, 42. ssh, scp and rsync -- ssh and rsync done, scp not

**Dropbear 2026.94 rather than OpenSSH**, and the reason is written
out in `ports/dropbear/build.sh`. OpenSSH separates privilege by
forking a child, setuid-ing it to a dedicated account and chrooting it
into an empty directory -- a design built on a filesystem that can
hold an owner and a permission, which this one cannot. Running it with
privilege separation off is exactly the configuration its authors warn
about. Dropbear was written for machines this size, carries its own
crypto so it need not agree with OpenSSL about anything, and bundles
scp. The protocol is the same protocol.

**Server password authentication is off.** Checking a password means
`crypt(3)` against a hash, picolibc has none, and inventing one badly
is worse than not having one. Public keys work, and are what should be
used anyway. `crypt()` belongs in the C library and is a task of its
own.

**What is demonstrated, by running it:** `dropbearkey` generates an
Ed25519 host key **on the 68040**; a real OpenSSH client on another
machine authenticates into the Sage040 by public key and runs `id`,
`uname` and `cat`, getting `uid=0(root) gid=0(root)` and `SuckOS`
back; and `rsync -rlt` over that ssh transfers a file into the machine,
verified by reading it back. rsync also copies locally, byte-for-byte.

**`rsync -a` reports `chown ... failed: Not owner`** and is right to:
`-a` asks for ownership to be preserved and there is nowhere to record
it. `-rlt` is the flag set that matches this filesystem.

**scp did not work in the one test made of it, and the cause is NOT
known.** What is known: the binary builds, installs and prints its
usage; `scp -O root@machine:/ST/file local` from an OpenSSH client
failed with "local/path: No such file or directory" while `ssh` and
`rsync` over the same transport, in the same session, worked.

**A correction, because the first version of this note drew the wrong
conclusion from a bad test.** It said the failure was guest-side,
because `scp -f FILE` run on the machine exits 1 immediately. That
proves nothing: `-f` is the source half of the scp protocol and the
first thing it does is `response()`, which reads a byte from a peer
that is supposed to be speaking scp. Run from a shell prompt with no
peer, exiting 1 is correct behaviour, not a fault. The probe was
invalid and the conclusion drawn from it was wrong.

Two things are worth knowing for whoever picks this up. A modern
OpenSSH client needs `scp -O` to use the old protocol at all, Dropbear
having no sftp-server. And a program built for this machine can now be
run under `qemu-m68k` (see below), which makes scp cheap to debug --
though only when linked statically, since a dynamic one asks for
`/lib/ld.so` and that loader reads the stack the kernel's way.

### 48a. The libraries CPython is built against -- done

Seven ports, each fetched at a pinned version and built into `~/m68k/src`
with nothing landing in this tree, and each wired into `make ports` (and
into a new `make pylibs`, which is what `make python` now depends on):

| port | what it gives CPython | on the disk |
|------|----------------------|-------------|
| **bzip2** 1.0.8 | `bz2` | `/BIN/bzip2` |
| **xz** 5.6.3 | `lzma` | `/BIN/xz` |
| **zstd** 1.5.7 | `compression.zstd`, new in 3.14 | `/BIN/zstd` |
| **SQLite** 3.53.4 | `sqlite3` | `/BIN/sqlite3` |
| **OpenSSL** 3.5.4 | `ssl`, and a `hashlib` whose digests come from OpenSSL | `/BIN/openssl` |
| **readline** 8.3 | line editing and history at the interactive prompt | a library only |
| **libffi** 3.5.2 | `ctypes`, as far as it goes without `dlopen` | a library only, with `fficheck` |

`kernel/pylibtest.sh`, 25 checks. **CPython is rebuilt against them**
(2026-09-23): 90 built-in modules where there were 87, gaining `_ssl`,
`_hashlib`, `_sqlite3`, `_bz2`, `_lzma`, `_zstd` and `readline`. Only
`_multiprocessing` and `_posixshmem` are still missing, both wanting
POSIX shared memory. `_ctypes` stays off and **not for want of
libffi**, which is built and works: `_ctypes.c` includes `<dlfcn.h>`
and opens libraries by name at run time, and this loader has no
`dlopen`. That is a missing loader feature, not a missing library. **Every stream crosses the host
boundary in both directions** -- the machine compresses and the host
decompresses, and the host compresses and the machine decompresses --
because a compressor tested against its own output is `t3-ata` again:
self-consistently wrong is still wrong, and these are byte streams with
a defined byte order on a big-endian machine. The digests are checked
against coreutils' `md5sum`/`sha1sum`/`sha256sum`/`sha512sum`, a
different implementation on a different CPU, and against the published
SHA-256 of the empty string, which is owed to nothing on this host at
all. AES-256-CBC is encrypted on the machine and decrypted on the host.

**Five things were wrong, and each was found by a test rather than
by reading:**

1. **`fdatasync` did not exist**, so every write to a SQLite database
   failed. The kernel had `fsync` (118) and not `fdatasync` (148);
   SQLite prefers `fdatasync` when it is available, got ENOSYS, and
   reported **"disk I/O error"** -- a message with no mention of
   syncing anywhere in it. Found by linking a five-line program against
   `libsqlite3.a` and printing `sqlite3_extended_errcode()`: 1034,
   `SQLITE_IOERR_FSYNC`. The kernel now answers 148 exactly as 118,
   because this filesystem has no separate metadata journal for the
   distinction to save.

2. **`fcntl(F_GETLK)` returned EINVAL.** picolibc's fcntl knew
   `F_SETLK` and not the other two. Fixed in `libc/patches/32`:
   `F_GETLK` answers `F_UNLCK` ("nothing would conflict"), which is the
   truthful answer on a system with no advisory locking, and `F_SETLKW`
   is `F_SETLK`'s. *This was not what broke SQLite* -- it was found
   while looking, fixed, and the database still failed. Worth saying,
   because stopping at the first plausible cause would have left the
   real one in place.

3. **`cacheflush(2)` did not exist**, and libffi would not compile
   without it. See the section below.

4. **libffi read a pointer return from the wrong register.** Its m68k
   code takes it from `%a0`, which is the m68k SVR4 convention (return
   in `%a0`, copy to `%d0` in the epilogue so undeclared callers still
   work). m68k-elf gcc 15.2.0 does not do that: it returns pointers in
   `%d0` alone and leaves `%a0` holding something else. So `ffi_call`
   on a function returning a pointer gave back `0x100004f0` -- an
   address inside the program's own text -- with nothing failing
   anywhere. `ports/libffi/patches/01` reads `%d0`, which is correct
   under both conventions because SVR4 puts the value there too.
   Closures were already right; they write both registers.

   This is the whole reason `ports/libffi/test/fficheck.c` exists.
   libffi BUILDING says only that its m68k backend compiles; whether
   the frames it lays out are the ones this compiler expects is a
   different question, and the answer was no. The eleven checks call
   functions whose arguments differ in width and value, so a frame
   built wrongly cannot come out right by luck, and the closure check
   hands a run-time-written function pointer to a call site that knows
   nothing about libffi -- which is also the first thing on this
   machine to execute code it generated itself.

5. **zstd's legacy decoders and OpenSSL's QUIC assist thread both
   assume `uint32_t` is `unsigned int`.** On `m68k-elf` it is
   `long unsigned int`, so `U32 *` and `unsigned *` are incompatible
   pointer types and neither file compiles. zstd's is dead code (the
   v0.1-v0.7 frame formats, unwritten since 2016) and is switched off;
   OpenSSL's is a one-line patch to use the typedef the API declares
   (`ports/openssl/patches/01`), correct on every platform.

**What the machine cannot afford:** `xz` at its default preset. LZMA's
memory use follows its dictionary size, and `-6` wants about 94 MB to
compress on a machine with 64 MB of RAM. It fails cleanly -- "Not
enough space", exit 1 -- rather than crashing, and the suite checks
that it does, so the low preset used elsewhere is not mistaken for
timidity. `-1` needs about 9 MB and works.

**OpenSSL needed a Configure target of its own**
(`ports/openssl/50-sage040.conf`): `linux-generic32` minus `-pthread`
(this gcc has no such option and refuses the whole compilation), minus
`-ldl` (there is no `dlopen`), minus `afalgeng`. `-DB_ENDIAN` is passed as well, and **is not load-bearing** -- a claim
made here in the opposite direction first, and then tested. The whole
of OpenSSL was configured a second time without the flag, built, and
run on the machine: it agrees with the flagged build and with the
host's coreutils on SHA-256 and MD5 alike. OpenSSL 3.5 works its own
byte order out. The flag stays because it is true and costs nothing.

So the negative control for the digests **did not fail**, and that is
the finding rather than a gap: the checks compare against a different
implementation on a different CPU, which is worth doing, but they are
not evidence about that flag and must not be read as any.

### cacheflush(2) -- done

m68k's own system call, number 123, which no other architecture has.
The 68040's data and instruction caches are separate, so a program that
writes instructions into memory and jumps to them has stored bytes that
may still be in the data cache while the instruction cache holds what
used to be there. Only the supervisor can do anything about it --
`cpusha` is privileged -- so the program has to ask.

libffi is the caller: it writes a closure trampoline and then calls
`SYS_cacheflush`. It would not build at all without `<asm/cachectl.h>`,
which is now in the C library at Linux's path, with Linux's constants.

`kernel/cache.c` has it, and is honest about what it does today: the
caches are OFF (nothing writes CACR), so there is nothing to push. The
instruction is issued anyway because it is correct and costs four
cycles, and whoever turns the caches on does not have to come back.
**It cannot be observed to work by running it** -- QEMU decodes
`cpusha`, `cpushl` and `cinv` as privileged no-ops, and on emulated
hardware with no caches a correct flush and a missing one look
identical.

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
