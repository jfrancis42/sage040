# Running GNU Emacs on Sage040

An assessment of what it would take to build GNU Emacs on a Linux host and
have the resulting executable actually run on this machine. Not a port
plan that hedges — a list of what is in the way, measured where measuring
was possible, with the guesses marked as guesses.

**Short answer:** current Emacs is out of reach, and the reason is not RAM
and not disk. It is the 2 MB user address space, the absence of any way
for a program to ask for memory, and a filesystem that cannot store 43% of
Emacs's own filenames. The first two are the same problem wearing
different hats, and fixing it is perhaps 800 lines of kernel work. The
third is a new filesystem.

**An older Emacs is genuinely within reach**, and that is the more
interesting finding. GNU Emacs 18 shipped on Sun-3 workstations — 68020,
4 MB of RAM — which is this machine with a slower CPU. That path is at the
bottom of this document.

---

## What was measured

All figures below are from GNU Emacs 31.1 as installed on the development
host (x86-64, glibc), because that is what could be put on a scale. Where
a number has to be translated to this machine it is marked as an estimate
and the reasoning is given.

| | |
|---|---|
| `emacs` executable | 3.7 MB |
| `emacs-<fingerprint>.pdmp` (the dumped Lisp image) | 17.6 MB |
| `lisp/` as `.elc` only | 69 MB, 1,640 files |
| `lisp/` as `.el` sources | 22 MB |
| `etc/` | 18 MB |
| `emacs -Q -batch`, idle: resident | **54.8 MB** |
| — of which anonymous data (heap) | 32.9 MB |
| — address space reserved (`VmSize`) | 184.7 MB |
| distinct system calls, `-Q -batch` | 38 |
| `mmap` calls during that run | 876 |

The 54.8 MB is a **floor**, not a typical figure. That is batch mode: no
terminal, no redisplay, no modes loaded, one small file visited. An
interactive session with a few buffers and a major mode is larger.

**Estimate for a 32-bit big-endian build: 25–35 MB resident.** The
reasoning: `VmData` is 32.9 MB and Emacs's heap is dominated by Lisp
objects, which are tagged machine words — conses are two words, and the
object header of every string, vector and symbol is word-sized. Halving
the word halves most of that. The 17.6 MB dump shrinks for the same
reason. This is an inference from how Emacs represents data, not a
measurement; a 32-bit build was not available to weigh.

---

## Blocker 1 — the address space

A program on this machine gets **2 MB**, mapped at exec and fixed:

```
0x10000000   text and data
     ...     2 MB total
0x101F0000   stack, 64 KB
```

Emacs needs somewhere between 25 and 55 MB. That is not a tuning problem
or a matter of trimming features; the executable alone is 3.7 MB and the
dump is 17.6 MB, so Emacs exceeds the entire address space before a single
Lisp object exists.

Nothing else in this document matters until this changes.

The good news is that the constraint is arbitrary. `USER_VA_SIZE` is a
constant in `vm.h` and `USER_PTABLES` is 8. The MMU is a full three-level
68040 unit with a 32-bit virtual address space; the page tables are built
by `vm.c` and there is no architectural reason a program cannot have
hundreds of megabytes. The 2 MB was chosen when a program was a small
thing loaded at a fixed address, and it has simply never been revisited.

**Physical** memory is not a constraint either: the machine model accepts
up to 2 GB and the default 4 MB is a default. `-m 256` works today.

---

## Blocker 2 — a program cannot ask for memory

There is no `mmap`, no `brk`, no `sbrk`, and therefore no `malloc`. A
program's pages are mapped at exec and the set never changes. `lib/ulib.h`
has no allocator at all — a program that wants memory declares an array.

Emacs is, to a first approximation, a program that allocates. It called
`mmap` 876 times in a six-second batch run that opened one file. Its
garbage collector, its buffer text (gap buffers that grow), its string
data, and the dump loader all rest on being able to get more memory and
give it back.

This cannot be worked around with a large static array. Emacs's allocator
can be pointed at a fixed region — `REL_ALLOC` and the old `gmalloc` exist
— but the region still has to be big, which returns to Blocker 1, and
`pdumper` maps the dump file, which returns to `mmap`.

**What is needed:** `mmap` and `munmap` for anonymous memory, at minimum,
plus `mprotect` if the dump loader's read-only phase is kept. `brk` alone
would do for a classic `sbrk`-based malloc and is perhaps 60 lines given
that `vm.c` already maps pages into an address space on demand from
`exec`.

---

## Blocker 3 — the missing system calls

Sage040 has 40 system calls. Emacs used 38 distinct ones in a *batch* run
and more interactively. The overlap is smaller than those numbers suggest,
because Emacs's 38 are the ones a real libc issues.

Taken from the trace, against `kernel/uapi.h`:

### Present and sufficient

`read` `write` `open` `close` `lseek` `stat` `unlink` `rename` `mkdir`
`rmdir` `chdir` `getcwd` `getpid` `uname` `kill` `ioctl` — and, usefully,
`TCGETS` / `TCSETS` / `TCSETSW` / `TCSETSF` and `FIONREAD` already exist,
which is most of termios.

### Missing and required

| Call | What Emacs needs it for | Cost |
|---|---|---|
| `mmap` / `munmap` / `mprotect` | everything; see Blocker 2 | moderate |
| `select` or `poll` / `ppoll` | **the event loop.** Emacs waits on the terminal, subprocess pipes and timers in one place | significant |
| `sigaction` / `sigprocmask` | Emacs installs 25+ handlers: `SIGWINCH`, `SIGCHLD`, `SIGINT`, `SIGHUP`, and `SIGSEGV` for stack-overflow recovery | **significant — see below** |
| `timer_create` / `timer_settime` | `run-at-time`, idle timers, auto-save, blinking | moderate |
| `fstat` | distinct from `stat`; Emacs stats open descriptors constantly | trivial |
| `dup` / `dup2` / `pipe` | subprocesses and redirection | small |
| `fork` / `execve` / `waitpid` | `M-x shell`, `M-x compile`, `call-process`, `grep` | moderate |
| `access` | permission probes; could map onto `stat` | trivial |
| `readlink` / `symlink` | `file-truename`; can return `EINVAL` honestly | trivial |
| `getuid` / `getpwuid` | `~` expansion, `user-login-name` | trivial |
| `umask` / `chmod` / `utime` | preserving mode and mtime on save | small |
| `TIOCGWINSZ` | window size; without it Emacs assumes 80×24 forever | trivial |

**Signal handlers are the hard one, and not for the obvious reason.** This
kernel has signals, but they have *default actions only* — a program
cannot install a handler, and there is no mechanism to. Delivering a
signal to user code means building a signal frame on the user stack,
returning to the handler in user mode, and providing a `sigreturn` that
unwinds it. That is real work in `trap.c` and `execasm.s`, and it is the
single largest kernel item on this list.

Emacs can be made not to *need* most handlers — but not `SIGCHLD` if it
runs subprocesses, and not `SIGWINCH` if the window may be resized.

### Missing and avoidable

`getrandom` (falls back), `futex` (no threads), `sched_getaffinity`,
`rseq`, `set_robust_list`, `prlimit64`, `pidfd_open`, `timerfd_create` —
all either optional or glibc bookkeeping that a purpose-built libc would
not issue.

---

## Blocker 4 — the filesystem

This is the one that surprised me, and it is the clearest answer to "does
it need a better filesystem?": **yes**.

FAT16 with 8.3 names cannot store Emacs's own Lisp tree. Measured against
the installed `lisp/`:

```
  .elc files that fit 8.3:      942
  .elc files that do NOT fit:   698   (43%)
```

`ansi-color.elc`, `completion-preview.elc`, `cus-start.elc`,
`composite.elc` — ordinary, central files. Emacs finds Lisp by constructing
a filename from a feature name and looking it up on `load-path`, so this
is not cosmetic: `(require 'ansi-color)` looks for `ansi-color.elc` and
there is no way to tell it otherwise short of renaming 698 files and
patching every `require` and `autoload` that names them.

Also missing, in descending order of how much it matters:

- **Long names.** VFAT long-name entries are the smallest fix — they are
  an extension to the same on-disk format, so `fs/fat16.c` could grow
  them without a new filesystem. Roughly 400 lines, and it buys the
  single biggest blocker on this list.
- **Symbolic links.** `file-truename` and `file-symlink-p` degrade to
  "never a link", which is survivable.
- **Permission bits and ownership.** `file-writable-p` becomes "always",
  backup files lose their modes, and `#lock` files do not work.
- **Sub-second and even reliable mtimes.** Emacs compares a buffer's
  recorded modification time against the file's to detect a file changed
  on disk. FAT's two-second resolution makes that check weak; it does not
  break it.
- **Atomic rename over an existing file.** Emacs's save is
  write-temp-then-rename. `rename` exists here; whether it is atomic
  against a crash is a different question, and FAT's answer is no.

**Capacity is fine.** The disk image is 100 MB with 103 MB free in the
test partition, and a minimal Emacs — executable, dump, terminfo and a
working subset of `.elc` — is around 25–30 MB. The full `.elc` tree at 69
MB also fits. Disk is not a constraint; only naming is.

---

## Blocker 5 — the terminal

Emacs drives a terminal through terminfo. It needs a terminal database to
know how to move the cursor, clear to end of line, set attributes and
scroll a region.

Two routes:

1. **Ship a terminfo file.** `ncurses` is a dependency, or the small
   subset of it Emacs uses is reimplemented.
2. **Build with a single hardcoded terminal.** Emacs supports this poorly
   but the display code is behind an interface.

Then there is what is on the other end. `fbcon.c` currently understands
carriage return, backspace, tab, newline, and just enough of `ESC[2J`,
`ESC[H` and `ESC[K` for `clear`. **Emacs would need real cursor
addressing** — at least `ESC[<row>;<col>H`, `ESC[K`, `ESC[J`, `ESC[m` for
attributes, and ideally a scroll region. That is a straightforward
extension of the ANSI state machine already in `fbcon.c`, perhaps 200
lines, and it would be worth having regardless of Emacs.

Over the serial line, the host's terminal emulator does this already, so
serial Emacs needs no work here at all.

`TIOCGWINSZ` is missing; without it Emacs assumes 80×24. The framebuffer
console is 80×30 at 640×480, so it would simply not use the bottom six
rows.

---

## Blocker 6 — building it, and the dump

This is the part that catches people out, and it is worth being precise.

Emacs is not built by compiling and linking. It is built by compiling and
linking `temacs`, then **running `temacs`** to load ~100 Lisp files and
dump the resulting heap:

```
temacs --batch --load loadup --temacs=pdump
```

That produces `emacs.pdmp`, which the real `emacs` loads at startup.
Emacs 27 and later use this "portable dumper"; earlier versions used
`unexec`, which had the binary rewrite its own data segment — considerably
worse for this purpose. The installed dump here is named
`emacs-651d5398…pdmp`, the fingerprint form, confirming pdumper.

So **the build requires running a target binary**. Three ways out:

1. **Run `temacs` under `qemu-m68k` user-mode emulation on the host.**
   That is Linux/m68k, not Sage040, so it needs a Linux syscall surface —
   which qemu-user provides. The resulting `.pdmp` is then copied onto the
   Sage040 disk. *This is the recommended route and is how buildroot and
   Nix cross-build Emacs today.* Whether a dump produced under Linux/m68k
   loads correctly on Sage040 is an inference, not a measurement: the
   dump is relocated at load and the ABI is the same, so it should, but it
   is the first thing to test and the first thing to suspect.
2. **Run `temacs` on Sage040 itself.** Circular — `temacs` needs
   everything Emacs needs.
3. **Build Emacs 18 or 19**, which dumped much smaller images and had far
   simpler requirements.

The cross toolchain itself is not a problem: `~/m68k/install` has GCC
15.2.0 and binutils 2.45 targeting `m68k-elf`, and Emacs is C99 with no
exotic requirements. Big-endian is fine — Emacs has always supported it.

---

## Blocker 7 — there is no libc

`lib/ulib.c` is a thin wrapper over the system calls plus `strlen`,
`strcmp`, `memset`, `memcpy` and some output helpers. There is no `stdio`,
no `malloc`, no `printf`, no `qsort`, no locale, no `setjmp`.

Emacs needs a C library. Emacs uses `setjmp`/`longjmp` (for its
non-local exits and, historically, for the garbage collector's register
scan), `printf` family, `strtol`, `qsort`, math functions, and a great
deal of `string.h`.

The honest path is **newlib** or **picolibc**, both of which are designed
to sit on a small syscall layer and both of which target m68k. That
converts "write a libc" into "write about fifteen syscall stubs", which is
the right trade. It also imposes the syscall list in Blocker 3, because
those stubs are what newlib expects.

---

## What it would actually take

In dependency order. The estimates are for the kernel side; each is a
guess based on the size of the comparable code already in the tree, and
should be read as ±50%.

| | Work | Estimate |
|---|---|---|
| 1 | Raise `USER_VA_SIZE` to 256 MB; grow `USER_PTABLES` to match | ~50 lines |
| 2 | `mmap` / `munmap` / `mprotect`, anonymous and file-backed | ~400 lines |
| 3 | Port newlib or picolibc onto the syscall layer | ~15 stubs + build work |
| 4 | Real signal delivery to user handlers, plus `sigreturn` | ~350 lines, `trap.c` + asm |
| 5 | `select`/`poll` over descriptors, with a timeout | ~250 lines |
| 6 | POSIX interval timers | ~150 lines |
| 7 | VFAT long filenames in `fs/fat16.c` | ~400 lines |
| 8 | `fstat`, `access`, `dup`, `dup2`, `pipe`, `umask`, `chmod`, `utime`, `getuid`, `TIOCGWINSZ` | ~300 lines total |
| 9 | `fork`-or-`posix_spawn`, `execve`, `waitpid` for subprocesses | ~300 lines |
| 10 | Full ANSI cursor addressing in `fbcon.c` | ~200 lines |
| 11 | terminfo, or a hardcoded terminal in the Emacs build | build-side |
| 12 | Cross-build, dump under `qemu-m68k`, copy the `.pdmp` | build-side |

Call it **2,500 lines of kernel** plus a libc port plus a build pipeline.
That is a large project but not an unbounded one, and items 1, 2, 4, 5 and
7 are all things this OS should arguably have anyway — Emacs is a
demanding but honest test of an operating system, which is most of why the
question is interesting.

A significant caveat: **eight tasks and eight descriptors per task** are
static limits. Emacs with subprocesses will want more descriptors than
eight. That is a constant, not a redesign.

---

## The other editors

"An Emacs on this machine" and "GNU Emacs on this machine" are very
different projects, and so is "an editor on this machine". Measured and
estimated side by side:

| | Binary | Resident | Heap | Dump step | Lisp |
|---|---|---|---|---|---|
| GNU Emacs 31 | 3.7 MB + 17.6 MB dump | **54.8 MB** | 32.9 MB | yes | yes |
| Vim 9 (full) | 5.0 MB | **7.5 MB** | **0.5 MB** | no | Vimscript |
| Vim, `--with-features=tiny` | ~0.5 MB (est.) | ~2 MB (est.) | small | no | minimal |
| uEmacs/PK | ~0.3 MB (est.) | ~1 MB (est.) | small | no | no |
| `mg` | ~0.3 MB (est.) | ~1 MB (est.) | small | no | no |

Vim's figures are measured on this host with `vim -u NONE` editing one
file. The others are estimates from source size and from what those
programs historically ran on, and are marked as such — none was built for
this comparison.

### Vim

**The single most useful measurement in this document is Vim's `VmData`:
536 KB.** Emacs's is 32.9 MB — sixty times larger. Vim's 7.5 MB resident
is almost entirely its own 5 MB of mapped executable text, not data it
allocated. An editor whose *working set* is half a megabyte is a completely
different proposition from one whose working set is thirty.

Vim's syscall surface is also much closer to what exists here. Traced over
an edit-and-save: 40 distinct calls, and the differences from Emacs matter
more than the count.

- **It uses `brk`, not just `mmap`.** So a classic `sbrk`-based `malloc` is
  enough — the simpler half of Blocker 2, roughly 60 lines of kernel.
- **No `fork`, `clone`, `pipe` or `execve`** for ordinary editing. Those
  appear only for `:!` and `:make`.
- **No `timer_create` or `timerfd`.** Emacs's idle timers have no
  equivalent requirement here.
- **One event-wait call, `pselect6`**, and only for keyboard input with a
  timeout. On a machine with a single input source that is `read` with a
  timeout, which `tty.c` can already express.

Still required and still missing: `brk`, `sigaction` (Vim catches
`SIGWINCH`, `SIGHUP` and deadly signals to preserve the swap file),
`fstat`, `access`, `chmod`, `fchdir`, `getuid`/`getgid`, `readlink`,
`unlink`, and terminal size. Plus a libc.

Vim also wants a termcap/terminfo database, though `--with-features=tiny`
plus a builtin terminal entry avoids it — Vim ships builtin entries for
`ansi`, `xterm` and others precisely so it can be built where no database
exists.

**Vim's filenames are its own**, not a 1,640-file Lisp tree, so the FAT16
naming problem mostly evaporates. The syntax and runtime files under
`/usr/share/vim` are 45 MB and *do* have long names, but a tiny build
without syntax highlighting does not need them.

**Historical precedent is strong.** Vim was written on and for the Amiga —
68000, and versions ran in well under a megabyte. Vim 4 and 5 ran on
Atari ST and MS-DOS 8088 real mode. A 68040 with a few MB is a *large*
machine by those standards.

### uEmacs

MicroEMACS — Dave Conroy's original, and the uEmacs/PK line that Daniel
Lawrence developed and that Linus Torvalds still maintains a fork of — is
around 15,000 lines of C with no extension language at all.

It is the **most tractable** of the three, for one specific reason beyond
its size: **it does its own terminal handling, with a compiled-in driver
per terminal type.** `ansi.c`, `vt52.c`, `tcap.c` and others are selected
at build time, so it can be built against a hardcoded ANSI terminal with
no termcap, no terminfo and no curses. That deletes an entire blocker.

It also has no dump step, no Lisp reader, no garbage collector, and a
modest appetite for `malloc`. It was written to be portable to small
machines and ran on MS-DOS in 256 KB, on the Amiga, the Atari ST and VMS.

**Estimate: uEmacs would run on Sage040 given a larger address space, a
`malloc`, and a libc — items 1, 2 and 3 of the work list, and nothing
else.** That is the shortest path in this document to a real editor on
this machine, and it is measured in weeks rather than months.

`mg` is comparable: a BSD-licensed Emacs-subset editor of similar size,
with faithful Emacs keybindings and no Lisp. It needs curses, where uEmacs
does not, which is the one thing that tips the choice toward uEmacs here.

### vi

The distinction worth drawing is between **vi** and **Vim**.

Original `vi` is a 1976 program that ran on a PDP-11 in a 64 KB address
space. It is smaller than any option above. But it is also inseparable
from `ex`, and both depend on **termcap and the BSD curses of their era**,
on `/tmp` for their recovery buffer, and on the terminal semantics of a
V7 tty driver. A modern reimplementation — `nvi`, or BusyBox's `vi` —
is the realistic target rather than the historical source.

**BusyBox `vi` deserves consideration and may be the single cheapest
useful editor available.** It is a few thousand lines, it writes ANSI
escapes directly with no termcap, and it is written specifically to run in
constrained environments. It would need little more than `malloc` and a
terminal that understands cursor addressing.

### What this changes

The ladder, revised, with the work-list item numbers from the previous
section:

1. **BusyBox `vi` or uEmacs** — items 1, 2, 3, plus ANSI cursor
   addressing in `fbcon.c` (item 10) if the screen rather than the serial
   line is the target. Weeks.
2. **Vim, tiny build** — the above plus `sigaction` (item 4) and the small
   syscalls of item 8. Months, and the result is a real editor most people
   would be content to use.
3. **GNU Emacs 18** — adds `select` and timers, and a larger address space
   still. Longer.
4. **GNU Emacs 31** — the whole list, and the filesystem work is
   unavoidable.

Note that **steps 1 and 2 need no filesystem work at all**, which moves
long filenames from "blocking" to "worth doing anyway".

## Verdict

**Current GNU Emacs will not run on Sage040 today, and the barriers are
real rather than incidental.** Ranked by how much they matter:

1. The 2 MB address space — Emacs needs 25× that, minimum
2. No `mmap`/`brk`, so no allocator at all
3. 43% of Emacs's Lisp filenames cannot exist on FAT16
4. No user-installable signal handlers
5. No `select`/`poll`, so no event loop
6. No libc

**RAM is not a blocker** — the machine model takes 2 GB and 256 MB is a
command-line flag. **Disk is not a blocker** — a minimal install is about
30 MB against 100 MB available. Both of those were the obvious suspects
and both are fine. What is actually in the way is the *shape* of the
process model and the filesystem, which is a more interesting answer and a
more actionable one.

Every item on the list is bounded work on code that already exists in this
tree. None of it requires rethinking the design.
