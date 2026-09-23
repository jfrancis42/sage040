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

**In hand now** (asked for 2026-09-22, evening): the whole remaining
list, in this order -- **49** (the native toolchain) first, then
**CPython rebuilt** against the new libraries, then `df`/`du`, then
home directories, users and `/etc/passwd` (33, 34, 35), then ssh with
scp and rsync (41, 42), then the remaining POSIX gaps (30), then
libiconv/gettext/libatomic (45, 46, 50), and **CLISP last** (48).

**Explicitly deferred**: task 36, a filesystem that can hold owners,
groups and permissions -- and only the parts of 34/35 that need it.
Users and `/etc/passwd` are being built on FAT; what cannot be done
without a better filesystem is ENFORCEMENT, and that waits.

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
| 51 | **`df` and `du`** | asked for 2026-09-22 | **done** -- `du` is sbase's (`-k -h -a -s -d -x`); `df` is new, with `-h`, `-k` and `-i`. 14 checks, numbers verified against the host's own `mdir` |
| 49 | **A native toolchain** | asked for 2026-09-22 | **in progress** -- binutils runs on the machine; gcc needs a C++ chain first |
| 41 | **ssh, client and server, with scp** | asked for 2026-09-22 | **mostly done** -- Dropbear 2026.94. A real OpenSSH client authenticates into the machine by public key and runs commands. `scp` builds and does not work; see below |
| 42 | **rsync** | asked for 2026-09-22 | **done** -- 3.4.1, over ssh, verified both locally and from another machine |
| 33 | **A home directory** | | **done** with 34/35 |
| 34 | **Users: a process belongs to one** | | **done** -- identity only; enforcement needs 36 |
| 35 | **/etc/passwd** | | **done** |
| 45 | **libiconv** | CLISP needs it, and so does anything that converts between character sets; picolibc has `iconv` headers but no converters worth the name | |
| 46 | **gettext** | CLISP needs it; message catalogues, and the `_()` every GNU program is written around | |
| 47 | **readline** | CLISP needs it, and it is what makes any interactive program's line editing behave; over the terminfo of task 39. **CPython is rebuilt once this exists** -- its `readline` module is what gives the interactive interpreter a line editor, and it is switched off now for want of the library | |
| 48a | **The libraries CPython wants** (asked for 2026-09-22) | **done** -- see below. bzip2, xz, zstd, SQLite, OpenSSL, readline and libffi all build and are installed by `make ports`. CPython is **not** rebuilt against them yet: that was asked to wait |
| 48 | **CLISP**, a Common Lisp | asked for 2026-09-22. GNU CLISP is C plus a bytecode VM and wants its own build Lisp, as CPython wants a build Python; SBCL is out (it compiles to native code and has no m68k backend), ECL is the other candidate (it compiles to C). Depends on 45, 46 and 47 | |

**Not to be built until asked** (2026-09-22): tasks 45, 46 and 48 --
libiconv, gettext and CLISP. The list is here so the work is decided;
the work itself waits.

**readline (47) is built**, ahead of that rule and deliberately: it is a
CPython dependency as much as a CLISP one, and 48a asked for the
libraries that make a better CPython. Nothing CLISP-specific was built.
**CPython has not been rebuilt against any of this yet**, because the
instruction was to build the dependencies and stop.

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

### 51. `df` and `du` -- done

`du` was already there: sbase's, with `-a -s -d depth -h -k -H -L -P
-x`. `df` is new -- `system/df.c`, installed as `/bin/df` -- with
`-h`, `-k` and `-i`. `kernel/dftest.sh`, 14 checks.

**The numbers are checked against the host's**, not against
themselves. `mdir` reports the free space on the same image before the
machine boots, and the two agreed to the kilobyte (62836 K each way).
The file sizes are deliberately awkward -- 300,000 and 70,000 bytes,
which are a whole number of neither kilobytes nor clusters -- so a
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

### 49. A native toolchain -- in progress

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

**What is left, and why the chain is longer than it looks:** GCC 15 is
written in C++, so a compiler that runs on this machine needs a C++
standard library that runs on this machine. And the existing cross
toolchain is C-only -- there is no cross `g++` at all. So the order is
a cross g++ (into a separate prefix, so the working C compiler
everything depends on is never at risk), then libstdc++ against
picolibc, then the native gcc, then gdb.

### 45, 46. libiconv and gettext -- ports written, not yet built

`ports/libiconv` and `ports/gettext` are written and their sources
fetched and **signature-checked against GNU's keyring** (both are Bruno
Haible's, which is a pleasant coincidence given he also maintains
CLISP). Neither has been built: the machine they build on was busy
compiling gcc, and starting another build beside it is what caused an
out-of-memory kill earlier in the night.

gettext is deliberately **the runtime only** -- libintl and
`gettext(1)`. The tools (xgettext, msgfmt, msgmerge) are a developer's
rather than a machine's: catalogues are compiled on a workstation and
the `.mo` files copied over. Building them would mean porting their
gnulib, which has been the most troublesome thing in this tree by a
distance -- gcc's bundled copy had to be switched off entirely
(`ports/gcc/patches/01`).

### The stdint deviation -- a decision to be made

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

**scp does not work and is the open item.** The binary builds and
installs, prints its usage, and `scp -f FILE` -- the source mode the
remote end runs -- exits 1 immediately with no output, on the machine,
with no network involved. So it is not the ssh transport: something in
scp's startup fails against this C library. It was left there rather
than guessed at. Note also that a modern OpenSSH client needs `scp -O`
to talk the old protocol at all, Dropbear having no sftp-server.

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
