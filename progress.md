# Progress

Working log for the port-enablement work: making Sage040 able to build
and run software written by other people, with a real editor as the
proof. `design.md` §11 is the reasoned list; this file is the order, and
the running state as it actually is.

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk — both can be increased
and have been.

**Status: task 0 done, 1 next. 1 of 23 complete.** 130 checks pass.

Entries below are filled in *when the work is finished and tested*, not
before. If a task says done, its tests pass.

---

## Order, and why

Sorted so each task makes the ones after it easier. Four dependencies
drive almost all of it:

1. **Address space before memory.** `brk` is meaningless inside 2 MB.
2. **Memory before a libc.** `malloc` needs pages to hand out.
3. **A libc before any port.** Nothing real builds without one.
4. **Signals and `select` before an editor.** `SIGWINCH`, and waiting on
   input with a timeout, are what an interactive program is built on.

| # | Task | Why here | State |
|---|------|----------|-------|
| 0 | Groundwork: bigger RAM and disk, per-task cwd, `fstat`/`access`/`dup` | small things everything else trips over | **done** |
| 1 | Grow the user address space | nothing else fits until this | todo |
| 2 | `brk`/`sbrk` | the smaller half of memory; enough for `malloc` | todo |
| 3 | `mmap`/`munmap`/`mprotect` | what a runtime and a libc expect | todo |
| 4 | `malloc` in `lib/` | so tasks 5–12 have something to test against | todo |
| 5 | Signals: `sigaction`, handlers, `sigreturn` | the largest kernel item; editors need it | todo |
| 6 | `select`/`poll` | the other half of an event loop | todo |
| 7 | Interval timers | depends on 5 | todo |
| 8 | Pipes, `dup2`, real redirection | see the note below — `>` is broken today | todo |
| 9 | Subprocesses: `fork`/`execve`/`waitpid` | `:!` and `:make` in an editor | todo |
| 10 | The rest of the socket API, and the signatures | before a libc is written against the old ones | todo |
| 11 | **VT102** emulation in `fbcon.c` | a full-screen program needs cursor addressing | todo |
| 12 | `TIOCGWINSZ` and `SIGWINCH` | depends on 5 and 11 | todo |
| 13 | A C library (picolibc or newlib) | the gate everything real passes through | todo |
| 14 | VFAT long file names | 8.3 decides what can be shipped | todo |
| 15 | `fsck`, and a clean-unmount flag | the machine cannot check its own disk | todo |
| 16 | Build and run uEmacs | the cheapest real editor | todo |
| 17 | Build and run vi | the other one | todo |
| 18 | A resolver | independent; do it when convenient | todo |
| 19 | TCP: window scaling, timestamps, SACK, keepalives, real `TIME_WAIT` | was "deliberately not doing"; now on the list | todo |
| 20 | Shared libraries | downstream of `mmap` and the libc | todo |
| 21 | Paging and swapping | downstream of `mmap` | todo |
| 22 | Interrupt-driven input, the NVRAM, the static limits | cleanup, any time | todo |
| 23 | Regression tests throughout | every task ships with its tests | ongoing |

Tasks 16 and 17 are the point of the exercise. Everything before them is
what they need.

**Tasks 19–21 were in a "still open, deliberately" section**, each
justified by the machine being small. It is not small now, so they are
on the list like everything else.

**VT102 rather than VT100** (task 11), decided on the way past: VT102
adds insert and delete of lines and characters — `ESC[L`, `ESC[M`,
`ESC[P`, `ESC[@` — to VT100's cursor addressing, and those four are
exactly what an editor uses to avoid repainting the screen when a line
is inserted or a character typed mid-line. On a console where every
glyph is 128 pixels drawn one at a time, that is the difference between
usable and not. `vt102` is also a type every termcap and terminfo
database already knows, so nothing has to be invented to describe it.

---

## Log

### 0. Groundwork — in progress

**The machine is bigger, and the caps that hid it are gone.** Done and
measured; this part is finished.

RAM is **64 MB** and the disk is **512 MB**. Raising the emulator's `-m`
alone did nothing, because three separate limits capped what the kernel
could see, and every one of them failed *silently*:

1. **`kernel.ld` put the supervisor stack at a hardcoded 4 MB**
   (`LENGTH = 4M`, `_stack_top = ORIGIN + LENGTH - 16`), and
   `start_memory()` ran the page allocator from `_end` up to *just under
   that stack*. So every byte above 4 MB was invisible however much the
   machine was given. The stack is now reserved inside the image, in a
   `.kstack` section below `_end`, and the allocator runs from `_end` to
   the end of real RAM.
2. **`probe_memory()` looped `for (mb = 1; mb < 64; ...)`**, so it
   reported 64 MB for anything larger. A limit reached looks exactly
   like an end found. Now 2048, which is the emulator's own cap.
3. **The `pmm` bitmap was a static array sized for 64 MB**, and
   `pmm_init()` *clamped* to it and then reported the clamped figure as
   the memory found. The bitmap now lives at the front of the pool it
   describes, with its own pages marked used — the usual bootstrap knot,
   untied by not allocating: the size is known from the range, so the
   bitmap is placed rather than allocated. One page per 128 MB.

Measured after, booting at four sizes:

| `-m` | pages the allocator got |
|---|---|
| 4 | 948 |
| 64 | 16,308 |
| 256 | 65,460 |
| 512 | 130,996 |

**Sizes live in `machine.conf` now.** `RAM_MB` and `DISK_MB`, in one
file, written so it is valid in both GNU make and POSIX shell — four
Makefiles include it and five test scripts source it. It was a literal
`-m 4` in eleven places, which is how the emulator came to be given more
memory than the kernel could see without anyone noticing.

The disk is a fresh 512 MB FAT16 with 8 KB clusters (65,370 of them,
inside FAT16's 65,524 limit). `fsck.fat` is clean, the kernel reads and
writes it, and programs load from it.

**The working directory belongs to a task now.** It was a single
`static struct dir cwd` in `fs/fat16.c`, so a `chdir` anywhere moved
every task, including the shell. The *storage* is in `struct task`
beside the descriptors and is inherited by `task_create()` and `exec`
the same way they are; the *meaning* stays in the filesystem, which is
what `vfs_cwd_ino()` / `vfs_cwd_set()` / `vfs_cwd_path()` are for — the
task layer holds a `u32` and does not know it is a cluster number, so
`fs/` still does not include `task.h`.

Task 0 is built by hand in `task_init()` rather than through
`alloc_task()`, so its cwd has to be set there too. Missing that gave
every task in the machine an empty working directory, because
everything inherits from it.

**`fstat`, `access`, `dup` and `dup2` added.** `fstat` needed a new op
on `struct file_ops`: describing an *open file* is a different question
from describing a path, and a ported program asks it constantly — to
size a file it is about to read, or to find out whether what it has is
a terminal. Every device implements it as `S_IFCHR`, which is what
makes `isatty()` work, and `isatty()` is what an editor checks before
it does anything at all.

`dup` and `dup2` turned out to be **already implemented** in `vfs.c`
with no system call wired to them, so they cost two lines each.

*Decision:* `access(X_OK)` is answered by the same first-four-bytes ELF
test that `exec` uses, not by a mode bit — this volume has none. That
way `access(X_OK)` and `exec()` can never disagree, which is the whole
value of the call. `W_OK` is always true and says so.

#### A latent bug, found by the per-task cwd and much worse than it

**`vfs.c` stripped the leading slash off every path before handing it to
the filesystem**, on the reasoning that the mounted volume *is* the root
so the slash says nothing. That was true while the root was the only
directory anybody could stand in.

`path_walk()` resolves a name with no leading slash **relative to the
current directory**. So `/etc/rc` arrived as `etc/rc` and meant
`/etc/etc/rc` from inside `/etc` — while working perfectly from the
root, which is where everything had ever been tested. Measured:

```
/$ cat /etc/rc          -> echo booted from /etc/rc
/etc$ cat /etc/rc       -> /etc/rc: no such file or directory
/etc$ /bin/ifconfig     -> command not found
```

An absolute path silently meant something different depending on where
the caller happened to be standing. `path_walk()` had handled a leading
slash correctly all along — it resets to the root and skips it — so the
fix was to delete `strip_root()` and pass the path through unchanged.

This was pre-existing and nothing had caught it, because the shell was
the only thing that ever called `chdir` and every test ran from `/`.
`fstest.sh` and `apitest.sh` both check it now.

#### Found along the way

**`>` redirection is broken for programs, and silently.** The shell
implements `>` and `>>` by swapping *its own* output descriptor, which a
spawned program never sees. Measured: `hello > /OUT.TXT` prints to the
terminal and leaves a zero-length file. That is worse than not having
redirection, because it looks like it worked. Fixing it properly needs
`dup2` on the child's descriptor table before it runs, which is task 8 —
so it is recorded there rather than patched around now. Three documents
claimed there was no redirection at all; they now say what actually
happens.

---

### A test suite for the ABI itself

`kernel/apitest.sh` is new, and it is where the porting work gets its
regressions. The other suites test subsystems — the filesystem, memory
protection, the editor, the network. This one tests the **system call
surface**: the calls that exist so other people's software will build
and run, which are individually dull and collectively the whole point.

Its checks are made by **programs** in `apps/`, not by the shell,
because the thing being demonstrated is that a program can do these
things. Each program prints `ok` or `FAIL` per line and the script
counts them and reports each under its own name, so adding a check is a
line of C rather than a line of shell.

It grows one group per task, and a group is only added once its calls
work — so a failure there is always a regression, never a thing not
written yet.

**130 checks across six suites now:** 12 device programs, 41 fs, 20
api, 29 edit, 15 vm, 13 net.

## Decisions worth knowing about

- **RAM 64 MB, disk 512 MB**, both raised deliberately and both now
  single-sourced in `machine.conf`. The emulator allows 2 GB and the
  kernel scales to it, so neither is a ceiling.
- **The kernel's boot stack moved into the image.** It was at a fixed
  address near the top of a 4 MB machine; it is now a reserved section
  below `_end`, which is what lets the allocator own everything above.
- **The pmm bitmap is placed, not allocated**, at the front of the pool
  it describes.
- **VT102, not VT100**, for the console — see above.
- **Nothing is on a "deliberately not doing" list any more.**

## Notes for later

- `hello > /OUT.TXT` must work by the end of task 8, and there must be a
  regression test for it. It is the clearest example in the tree of a
  feature that looks present and is not.
