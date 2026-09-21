# Progress

Working log for the port-enablement work: making Sage040 able to build
and run software written by other people, with a real editor as the
proof. `design.md` §11 is the reasoned list; this file is the order, and
the running state as it actually is.

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk — both can be increased
and have been.

**Status: 3 of 23 complete.**

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
| 1 | Grow the user address space | nothing else fits until this | **done** |
| 2 | `brk`/`sbrk` | the smaller half of memory; enough for `malloc` | **done** |
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
| 18 | A resolver (DNS), then **NTP** | independent of the editor work; both are UDP clients and NTP wants a name | todo |
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

### 0. Groundwork — done

**The machine is bigger, and the caps that hid it are gone.**

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

**164 checks across six suites now:** 12 device programs, 41 fs, 51
api, 29 edit, 18 vm, 13 net.

### 1. Grow the user address space — done

**2 MB → 256 MB.** `USER_VA_BASE` stays at `0x10000000`; `USER_VA_END`
is now `0x20000000`. The stack moved to the top of the new space and
grew from 64 KB to **1 MB**, leaving ~255 MB of unmapped gap between a
program's image and its stack for `brk` and `mmap` to fill.

**What made 2 MB the limit was not the number, it was the bookkeeping.**
An address space's root table, pointer table and eight page tables were
packed into ONE physical page — root at `0x000`, pointer at `0x200`,
eight 256-byte page tables from `0x400`. Creating a space was one
allocation and destroying it one free, with no table management at all.
That was a good trade when a program was a small thing at a fixed
address, and eight page tables is exactly what fits in a page.

256 MB needs up to 1024 page tables, and allocating them eagerly would
cost 1 MB of tables per task on a machine that had 4 MB until this
morning. So **tables are allocated on demand**:

- a table page is taken from the page allocator when one is needed
- its first 512-byte slot holds a link to the previous such page, so
  they form a list the address space can free later
- the remaining seven slots are handed out to tables

`as_pagetable()` gained a `create` flag, and **the flag is the whole
difference between mapping and translating**: a map must be able to
bring tables into existence, and a translate must never do so —
otherwise dereferencing a wild pointer would quietly allocate the
tables to describe it, and a program could exhaust memory with rubbish.

`vm_mapped_pages()` and `vm_destroy()` now step a page table at a time
when a table is absent rather than a page at a time. Over 256 MB that
is 1024 iterations instead of 65536, and `free` calls it at every
prompt.

Measured: a 256 MB address space boots, a program costs 262 pages --
its 1 MB stack, eagerly mapped, plus image and tables -- and `ps` and
`free` agree. All 130 existing checks pass unchanged.

*Decision:* the stack is 1 MB and **eagerly mapped**, not lazily grown.
There is no demand paging (task 21), so a stack can only be as big as
what is mapped at exec. 1 MB per task against 64 MB of RAM and eight
task slots is 8 MB worst case, which is the "do not economise" choice
and removes stack overflow as a thing ported software can trip over.

#### A leak found on the way

**Killing a background job did not give its memory back.** `task_reap()`
frees the address space, the kernel stack and the task slot, and it is
only called from `waitpid` — which the shell only ran from `jobs` and
after `fg`. So a background job that ended, or was killed, held all
three until somebody happened to ask for a listing.

That was survivable when an address space was 2 MB of tables in one
page. With a 256 MB space it is a megabyte a time, and with `TASK_MAX`
of 8 it is a machine that stops being able to start anything after a
few. The shell reaps at **every prompt** now, which is where a real
shell would do it on `SIGCHLD` — and signal handlers are task 5, so
this gets revisited then.

`vmtest.sh` checks it now: `free`, `spin &`, `free`, read the real pid
from `ps`, `kill -9` it, return to the prompt, `free`. Measured used
pages 37 → 299 → 37. **With the reap line commented out the check
fails** (299 afterwards), so it tests the fix rather than passing
regardless. 133 checks.

#### The session that stopped, and why

Partway through this task every shell command began returning 1 with
no output, and a restarted session did the same. It looked like
`exec` failing. It was not: **`/tmp` is a tmpfs with a per-user quota,
and ~5.6 GB of 512 MB disk images in old scratch directories under
a directory in `/tmp` had filled it.** The command runner writes each
command's output to a file there, so every command ran and its output
had nowhere to go. `df` showed 1.6 GB free, because a quota is not a
full filesystem. Deleting the images fixed it. `/tmp/sage040-cleanup.sh`
does that if it happens again. **Keep 512 MB disk copies in `scratch/`,
which is on `/home`, never under `/tmp`.**

### 2. `brk`/`sbrk` — done

`__NR_brk` is Linux's **45**, with Linux's convention: `brk(addr)`
returns the new break on success and the **old** one on failure,
never an errno, and `brk(0)` asks. Every Linux `malloc` detects failure
by comparing, so any other convention would break all of them.

The break belongs to the address space (`brk_start`, `brk_cur` in
`struct addrspace`), not the task, because it describes what is
mapped. `exec` sets it to the page after the highest `PT_LOAD`. Page
aligned, so the last page of `.bss` belongs to the image and a shrink
can never unmap part of the program. It may grow to one guard page
below the stack (`USER_BRK_LIMIT`).

`vm_unmap()` is new; nothing could take a page out of a user address
space before. It flushes **before** the page can be reused, not after.

In `lib/`: `brk()` returns 0 or `-ENOMEM`, `sbrk()` returns the old
break or `(void *)-1`, and the library caches the break the way glibc
does. `ulib.h` now includes `errno.h`, so programs get the error
*names*, which they never had. It is pure macros, and the layering
check already allowed it. `sysinfo()` got a wrapper too.

*Decision:* a request the machine plainly cannot meet is **refused up
front**, before any page is mapped. The first version mapped until
the allocator ran dry and then rolled back. That worked, but the page
tables built on the way stayed with the address space until it died,
so every failed `sbrk` cost the machine ~37 pages for nothing. The
rollback is still there as a safety net. Nothing can reach it today,
because the kernel is not preemptible and nothing allocates between
the check and the mapping.

**Growth refuses to pass over a page that is already mapped.** Nothing
maps in the gap yet, but `mmap` will. A heap that grew over a mapping
would silently replace its pages.

**Tests:** `apps/memtest` makes 26 checks and `apitest.sh` three more:
where the heap starts, growing, zeroed new pages, an unaligned break,
shrinking giving pages back to the machine, regrown pages being NEW
(zeroed) pages, refusals leaving the break alone, a request for more
than the machine has returning *every* page, an 8 MB heap, and a
program touching the page its own shrink gave back and dying for it.
With the up-front refusal disabled, the "every page returned" check
fails, so it tests something real.

---

## Design notes for the tasks not yet started

Written while the shell was down, because thinking does not need one.
These are decisions, not code — the point is that the next session
starts by typing rather than by deciding.

### 3. `mmap`/`munmap`/`mprotect`

Numbers: **90**, **91**, **125**. Note that Linux's i386 `mmap` at 90
takes a *pointer to an argument block* rather than six registers —
`old_mmap`. Do not copy that: this ABI has `d1`–`d5` and six arguments
do not fit, so pass a pointer to a small struct and **say so in
`uapi.h`**, since it is a deliberate divergence from a number that is
otherwise Linux's.

Anonymous memory first — that is what an allocator and a runtime
actually use. `MAP_FIXED` honoured; without it, place the mapping in
the gap between the break and the stack, searching upward from a
per-address-space hint.

File-backed mappings **read-only and eager**: read the file's pages at
`mmap` time rather than faulting them in, because there is no demand
paging (task 21). `MAP_SHARED` on a file should be **refused with
`-ENODEV`**, not quietly treated as `MAP_PRIVATE` — a shared mapping
that does not share is the kind of lie that costs somebody a day.

A per-address-space table of mappings is needed so `munmap` can find
one and so `vm_destroy` can free them. 32 entries is plenty. `munmap`
of part of a mapping has to split it.

**`mprotect` must `pflusha`.** A missing flush does not fail at the
`mprotect` — it fails several accesses later on a page whose old
descriptor is still cached, which is the hardest failure in this tree
to diagnose and is already written up in the working notes.

### 4. `malloc` in `lib/`

First fit over `sbrk`, coalescing on free, free list threaded through
the blocks. `malloc`, `free`, `realloc`, `calloc`. Perhaps 250 lines.

*Decision made in advance:* **write it, even though task 13 brings a
libc that has one.** Tasks 5–12 all want an allocator to test against,
and waiting for the picolibc port would block every one of them behind
the largest single piece of work on the list. It is meant to be thrown
away.

`realloc` should grow in place when the next block is free. That
matters more than it sounds: an editor's gap buffer grows by
reallocating, and copying a few hundred KB on every insertion is
visible on a 25 MHz machine.

Test it with a randomised allocate/free workload that checks free-list
integrity, not just with a few calls.

### 5. Signals: `sigaction`, handlers, `sigreturn`

The largest kernel item on the list, and the one that changes the most.

Numbers: `sigaction` **67**, `sigreturn` **119**, and prefer
`rt_sigaction` **174** if the mask needs to be wider than 32 bits — it
does not here, so 67 is enough and simpler.

Delivery: build a frame on the **user** stack holding the saved
registers and the old mask, point the return address at a small
trampoline, and `rte` to the handler in user mode. The trampoline calls
`sigreturn`, which restores everything from that frame. The handler
runs on the user stack, in user mode, and can itself be interrupted.

*Decision:* **the trampoline belongs in `lib/crt0.s`**, with its
address passed to the kernel by `sigaction` — not written onto the
user's stack by the kernel. Writing instructions to the stack would
require the stack to be executable, and this MMU can mark it not;
giving that up to save eight bytes of `crt0.s` is a bad trade.

`SA_RESTART` matters more than it looks: without it every ported
program needs `EINTR` handling on every call, and most do not have it.

This also lets the shell stop reaping at the prompt and reap on
`SIGCHLD` instead, which is where task 1's leak fix really belongs.

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
