# Progress

Working log for the port-enablement work: making Sage040 able to build
and run software written by other people, with a real editor as the
proof. `design.md` §11 is the reasoned list; this file is the order, and
the running state as it actually is.

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk — both can be increased
and have been.

**Status: 9 of 23 complete.**

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
| 3 | `mmap`/`munmap`/`mprotect` | what a runtime and a libc expect | **done** |
| 4 | `malloc` in `lib/` | so tasks 5–12 have something to test against | **done** |
| 5 | Signals: `sigaction`, handlers, `sigreturn` | the largest kernel item; editors need it | **done** |
| 6 | `select`/`poll` | the other half of an event loop | **done** |
| 7 | Interval timers | depends on 5 | **done** |
| 8 | Pipes, `dup2`, real redirection | see the note below — `>` is broken today | **done** |
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

**387 checks across six suites now:** 12 device programs, 41 fs, 270
api, 29 edit, 18 vm, 17 net.

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

### 3. `mmap`/`munmap`/`mprotect` — done

**The ABI is Linux/m68k's, shapes as well as numbers.** The design note
here said to depart from Linux at 90, on the grounds that six
arguments do not fit in `d1`–`d5`. That was wrong: Linux/m68k passes a
sixth argument in **`a0`**, and 90 on Linux/m68k *is* `old_mmap` with a
pointer to an argument block. So both exist, as Linux has them:
`mmap2` (192), six registers with the offset in pages, and `old_mmap`
(90), a `struct mmap_arg_struct` pointer with the offset in bytes. The
trap gate now passes `a0` to every call as the sixth argument, which
cost one push and four bytes of stack. `munmap` is 91 and `mprotect`
125. That glibc's m68k `sysdep.h` uses `a0` this way is from memory,
not verified against a copy here.

**There is no table of mappings.** The note planned a 32-entry table
per address space, with splitting on partial `munmap`. The page tables
already record, for every page, whether it is owned and what may be
done to it, so a second record would only be something to disagree
with. Partial `munmap` and partial `mprotect` need no splitting, and
`vm_destroy` frees mapped pages exactly as before. The cost: nothing
can list which `mmap()` call a page came from. Nothing asks yet.

**The one state the hardware has no descriptor for is `PROT_NONE`:** a
page the program owns but may not touch. It is an invalid descriptor
(type 00) that keeps the physical address and sets a software bit
(`DESC_SW_NONE`, bit 11). The MMU ignores everything in an invalid
descriptor except its type, so the page faults like an unmapped one,
while `vm_destroy`, `vm_mapped_pages` and `vm_unmap` still treat it as
owned. The contents survive a round trip through `PROT_NONE`.

**Placement is top down from just below the stack's guard page**, and
never below the current break, the layout Linux uses for the same
reason: the heap grows up, mappings grow down, and each gets the whole
gap. A free hint is honoured. `MAP_FIXED` discards what was there, and
`MAP_FIXED_NOREPLACE` refuses with `EEXIST`.

*Decision, reversing the note:* **`MAP_SHARED` on a file is allowed
read-only**, and refused with `-ENODEV` only when `PROT_WRITE` is
asked for. File mappings are copies made at `mmap` time (no demand
paging until task 21). A writable shared copy would silently not write
the file, and that is the lie worth refusing. A read-only one differs
only in not seeing somebody else's later writes, and reading a file
through `MAP_SHARED|PROT_READ` is common enough that refusing it would
break ported programs for no real gain. Anonymous `MAP_SHARED` is
accepted and is private in effect. That is exact until there is
`fork` (task 9), which has to revisit it.

`PROT_EXEC` is accepted and means nothing. **The 68040 page descriptor
has no execute bit**, so anything readable is executable. That also
corrects a claim in the task 5 note below.

**Tests:** `memtest` now makes 77 checks, and `apitest.sh` has 107 in
all. They cover anonymous and file mappings, partial `munmap`,
`mprotect` both ways including `PROT_NONE` with contents preserved,
the kernel refusing to `read()` into a read-only page or `write()` from
a `PROT_NONE` one (`EFAULT`), hints, `MAP_FIXED` and
`MAP_FIXED_NOREPLACE`, the heap refusing to grow over a mapping, every
refusal and its errno, `old_mmap` through its block, a file mapping
outliving its descriptor, and the descriptor's file position left
alone. Three deliberate faults are checked from the shell: touching an
unmapped page, writing a read-only one, reading a `PROT_NONE` one.
`free` is checked before and after a `memtest` that exits with a
`PROT_NONE` mapping and a file mapping still in place: 37 pages used
both times.

**The flush is tested.** With the `pflusha` removed from
`vm_protect()`, the write to a read-only page goes through and three
checks fail. That is exactly the stale-descriptor failure the working notes
warns about, now caught by a test instead of by an afternoon.

### 4. `malloc` in `lib/` — done

`lib/malloc.c`: `malloc`, `free`, `calloc`, `realloc`, plus
`malloc_check()` and glibc's `mallinfo()`. **It is written to be
thrown away when the C library arrives (task 13),** as decided in
advance. It exists so that tasks 5–12 do not wait on the largest item
on the list.

The note planned first fit over one free list. What was built is a
little more, each piece for a reason:

- **Boundary tags** in the dlmalloc style: a 4-byte header, a footer
  only on free blocks, and a "previous in use" bit in the *next*
  block's header. Every merge is O(1), and a block in use costs 4
  bytes.
- **Segregated free lists,** one per power of two, first fit within a
  list. A single list makes every `malloc` walk every free block, which
  an editor making thousands of small allocations would feel.
- **Requests of 128 KB and up come from `mmap`,** as glibc does, and
  `free` gives them straight back with `munmap`. That is what makes
  loading a big file and letting it go actually return the memory.
- **The top of the heap is trimmed** with a negative `sbrk` once more
  than 256 KB there is free.
- **`realloc` grows in place,** into a free neighbour or, for the last
  block, by moving the break. The note asked for this because an
  editor's gap buffer grows by reallocating.
- **A double free, or a pointer `malloc` never returned,** is reported
  on stderr and ends the program with status 134, which the shell reads
  as `SIGABRT`. Carrying on would corrupt the lists.
- **A segment starts on an 8-byte boundary** even when the program has
  moved the break to an odd address itself. Found on review, not in a
  test.

**Programs are now linked with `--gc-sections`.** Every program compiles
the whole library, so without it every one of them would carry the
allocator. With it, every program *shrank* by about 3 KB, because
unused `ulib` functions go too.

**`lib/user.ld` still said `LENGTH = 2M - 64K`**, missed in task 1. It
would have refused to link any program bigger than about 1.9 MB,
exactly the kind of program the porting work is for. It is now
`256M - 1M - 4K`, which is everything up to the stack's guard page.

**Tests:** `apps/malloctest`, 27 checks. The fixed ones cover
alignment, `malloc(0)`, `free(NULL)`, `calloc` zeroing reused dirty
memory, `calloc` overflow, an impossible request returning NULL,
`realloc` preserving contents and growing in place, the `mmap` path
out and back, and trimming. Then the randomised workload: 40,000
operations across 512 slots, sizes mostly small with some over 128 KB.
Every block carries a pattern naming its slot, checked before every
free and after every `realloc`, and `malloc_check()` walks the whole
heap every 500 operations. At the end everything is freed, and nothing
may remain in use. `apitest.sh` checks the double free from outside,
and **waits for the workload to finish rather than sleeping past it**,
because how long 40,000 allocations take depends on the host. With
backward merging disabled in `coalesce()`, `malloc_check()` reports
"two free blocks are adjacent" and six checks fail.

### 5. Signals — done

#### Found first: the system call gate read `a6` as the status register

`_trap0_entry` saved `d1`–`a6` with one `movem`, a comment that said
"13 registers, 52 bytes", and then read "the saved SR" from `56(%sp)`.
**It is fourteen registers, 56 bytes.** Past those and the one pushed
word, offset 56 is the saved `a6`. Measured by logging what the
dispatcher received: every call from the shell arrived with "SR" 0 or
7, where the real value always has the supervisor bit (`0x2000`) set.
**So every system call a kernel task made was taken for one from user
mode**, and on the way out it had signals delivered and could be
preempted. the working notes and the kernel README both say kernel tasks are
never preempted. That was not true.

It never showed, because the shell ignores SIGINT and SIGTSTP, and the
one signal it does get, SIGCHLD, defaults to being ignored, so wrongly
"delivering" it just discarded it. It had to be found now because
signal handlers are started by rewriting the saved registers, which
this gate did not even hold all of.

**The fix makes every way into the kernel save the same thing.** The
gate saves all fifteen registers, `d0` included, in the layout the
MFP interrupt stub and the exception path already used, and passes a
pointer to them: `struct pt_regs` in `ptregs.h`, as on Linux. The
result goes into the saved `d0`. `task_ret_to_user()` takes the
`pt_regs` and reads the real SR from it. The MFP stub, which had the
offset right, passes the same pointer.

**Correcting it exposed the policy the bug had been standing in for.**
A kernel task never returns to user mode, so a signal pending on one
would never be acted on, and every sleep it attempted would return
`EINTR` for ever. Two rules, both Linux's:

- **A kernel task takes no signals.** `signal_send` drops them, and the
  `kill` system call refuses one aimed at a kernel task with `EPERM`.
- **An ignored signal is discarded when it is sent**, not left pending.
  That covers one the task asked to ignore and one whose default is to
  be ignored, like SIGCHLD. A blocked one is kept, since its
  disposition may change before it is unblocked.

**Tests:** `apps/sigtest` checks that a hand-written `trap #0` returns
`d1`–`d7` and `a0`–`a6` unchanged and the result in `d0`, and
`apitest.sh` checks that `kill 2` and `kill -9 2` (the shell) are
refused. Neither would have failed on the old gate. **The bug itself
has no lasting regression test,** because the only symptom was a
kernel task being preempted at a system call's exit, and nothing
outside the kernel can observe that. It was found and confirmed by
measurement.

#### Found second: nothing saves the FPU across a context switch

`switch_context` saves the integer registers and USP, and no code
anywhere in the kernel touches the FPU. Two tasks using floating point
share one set of FP registers and one rounding mode. It has not shown
because the cube is the only FP program. A signal frame needs the same
save, so it is fixed as part of this task.

**Fixed:** every task has a 208-byte FPU area, and `schedule()` saves
the outgoing task's FPU and restores the incoming one's: `fsave`, then
`fmovem` of `fp0`–`fp7` and the three control registers unless the
frame is null, and the reverse on the way in. **QEMU's `fsave` always
writes an idle frame and its `frestore` does nothing**, so a new task
starts with a fabricated idle frame and zeroed registers. A null frame
would have let it inherit the previous task's registers under QEMU.

**Test:** `apps/fptest N` fills all eight FP registers and `fpcr` with
values derived from N, then for three seconds yields and spins,
checking the registers as raw bits (a `double` comparison would use
the registers it is checking). `apitest.sh` runs two at once. The
first version ran a fixed number of rounds and **finished before the
second copy started**, so it passed without testing anything. Timing
by the clock fixed that. With the save removed, both copies report
lost state, the background one exactly when the foreground one starts.


#### Handlers

**The ABI is Linux/m68k's old signal interface:** all 31 signal numbers;
`sigaction` (67) with m68k's field order (handler, mask, flags,
restorer); `sigprocmask` (126), `sigpending` (73), `sigsuspend` (72,
one argument), `pause` (29) and `sigreturn` (119); 32-bit masks with
signal N in bit N−1. The kernel's own `SIGMASK()` changed to that layout
so masks cross the boundary unconverted. The `rt_` calls with 64-bit
masks are not here. Nothing has more than 31 signals to describe, and
the libc will get whichever stubs task 13 writes. Which form of
`sigsuspend` m68k uses (one argument or three) is **not verified**;
modern libcs use `rt_sigsuspend`, so it matters little.

**The frame** on the user stack is: return address (the restorer), the
signal number, a code of 0, a pointer to the context, then a `struct
sigcontext` holding every register, the old mask, the user stack
pointer, sr, pc and the whole FPU state. A handler is an ordinary
one-argument function. It returns to `__sigreturn_trampoline` in
`crt0.s`, and that trampoline's `sigreturn` restores everything. **Only
the condition codes of the saved sr are honoured.** The S bit in
particular is the kernel's, which is what stops a forged frame reaching
supervisor mode. `sa_restorer` is **required** for a handler, and the
library always supplies it; the kernel will not write code onto a stack.

*Correction to the note that planned this:* it gave "this MMU can mark
the stack non-executable" as the reason for the trampoline. The 68040
has no execute bit. The trampoline stays in `crt0.s` for other reasons:
it is what Linux does with `sa_restorer`, code on the stack needs cache
pushes on a real 68040, and a trampoline in source can be read.

**Restarting an interrupted call.** Blocking calls return `-EINTR`, and
`pause`/`sigsuspend` return an internal `-ERESTARTNOHAND`. On the way
out, if no handler ran (a stop and continue, say), the call is
restarted invisibly: the number goes back in d0 and the pc steps back
over the 2-byte trap. After a handler it restarts only with
`SA_RESTART`, and never for `pause`/`sigsuspend`. So a `read` suspended
by ctrl-Z and resumed by `fg` carries on reading, as on Linux. The
dispatcher records the call number in `current->syscall_nr` so this is
decided once, only for calls.

**Also fixed or added on the way:** SIGKILL resumes a stopped task (it
used to sit pending in a task that would never run); SIGSTOP, SIGTTIN
and SIGTTOU stop; SIGCONT resumes when *sent*, even if caught or
ignored; a child's exit now sends SIGCHLD (only stops used to);
`SA_NOCLDSTOP`, `SA_NODEFER` and `SA_RESETHAND` work; `kill(pid, 0)`
reports whether a task exists. In `ulib`: `sigaction`, `signal` (with
`SA_RESTART`, as glibc), the mask calls, `pause`, `kill`, `raise`,
`getpid`, `waitpid`, `spawn` and the `sigset` helpers.

**Not supported, and refused rather than half done:** `SA_SIGINFO` and
`SA_ONSTACK`, both `EINVAL`. **A fault's own signal cannot be caught**:
an access fault pushes a format-7 frame that re-runs the access on
`rte`, and it cannot be redirected to a handler in place. Faults still
end the program.

#### Two more bugs found by the tests, both older than this task

**A signal to one sleeping task woke every task in `nanosleep`.** All of
them sleep on one shared queue, and `signal_send` woke the target with
`wake_all(t->queue)`. The others found no signal and returned 0 with
their sleep cut short, silently. `wake_signalled()` in `wait.c` now
takes exactly one task off its queue.

**A program that spawned another made every task read the wrong
memory.** `exec_spawn` saved `uaccess_current()` and restored it
afterwards, but for a *program* that is its own address space, not "no
override". So the restore installed the spawning program's address
space as a global override. From then on every task's system calls,
including the shell's, read and wrote the spawner's memory. The child's
output was the parent's data, and its `nanosleep` read a time off the
parent's stack: a twelve-hour sleep. It healed the next time the shell
spawned anything, which is why nothing had noticed: until now no
program spawned another. Found by bisecting across three kernels: it is
present at the task 0 commit. `uaccess_set()` now returns the raw
previous setting, and that is what gets put back.

**Also found:** `exec_spawn` gives the terminal to every child it
starts, which is a shell's decision built into the kernel. A program
that spawns a helper loses its own terminal to it. **And nothing reaps
an orphan:** a program that exits without waiting for its children
leaves them as zombies holding task slots for ever. Both are for task 9
(`fork`/`execve`), noted there.

**Tests:** `apps/sigtest`, 32 checks. They cover handlers, the mask
while a handler runs and with `SA_NODEFER`, `sa_mask`, `SA_RESETHAND`,
blocking and `sigpending` and unblocking, ignore discarding a pending
signal, the refusals, `kill(pid, 0)`, and a hand-written trap whose
every integer and FP register (and `fpcr`) must survive a handler that
wrecks them all. Then `pause`, `SA_RESTART` against a real interrupted
`nanosleep` (EINTR at 200 ms without it, a full 1 s with it),
`sigsuspend` with its mask put back, and SIGCHLD from a child's exit,
all using a helper that signals from another task. `apitest.sh` adds:
a second sleeper that must sleep its full time, ctrl-C reaching a
handler both in `pause` and in a loop that makes no system calls, a
signal whose frame cannot be written ending the program, a forged
`sigreturn` asking for supervisor mode ending in a privilege
violation, and SIGSTOP then SIGKILL on a stopped job.

**Negative control on the forgery check:** with `sigreturn` honouring
the whole saved sr, the forged context ran in supervisor mode and took
the machine down. The first version of the check **passed anyway**,
because it only looked for the absence of a message that the crash
prevented from printing. It now requires the privilege violation.

**The harness trap in the working notes bit again:** ctrl-C was written into
the pre-built session and arrived, in the same burst as everything
else, while an earlier program was running. Keys that must arrive at a
particular moment are now sent by `send_after`, which waits for the
program to say it is ready.

### 6. `select`/`poll` — done

Linux/m68k's three calls: `poll` (168), `_newselect` (142) and the old
`select` (82), which takes a `struct sel_arg_struct` pointer, as 82
does on Linux/m68k. `fd_set` is Linux's 1024 bits. `select` writes the
unused time back, as Linux's does. Both are interrupted by a signal
through the task 5 machinery: `-EINTR` after a handler, a restart if
none ran. The restart begins the timeout again rather than resuming it,
since there is no restart block; Linux would resume.

**Readiness is a new `file_ops->poll`**, and when a file has none,
`FIONREAD`: readable when bytes are waiting, always writable. That rule
makes the terminal, the serial port and the keyboard right with no new
code. A file that cannot answer `FIONREAD` is a regular file, and
reading one never waits, so it is always ready. Only sockets needed a
`poll` of their own (`sock_poll`, over a new `tcp_poll`). A listener is
readable when `accept` would not wait. A connection is readable at end
of stream or on a reset, not only when data is waiting, because a
program must be told so it can make the read that returns 0.

**How it waits** differs from the design note, which had a task
registered on several wait queues at once. There is **one shared queue
that the terminal wakes from the tick**, and the loop also sleeps with
a short timeout of its own: 100 ms, or 20 ms when a socket is watched.
The network stack does its protocol work only when somebody asks, so a
socket has nothing that could wake a queue until the poller asks
anyway.

*Limit, documented in `uapi.h`:* a terminal in **canonical** mode is
readable when a character is waiting, not when a whole line is. The
line is assembled inside `read()`, so nothing outside it knows where
one ends. In raw mode it is exact, and raw mode is what a program that
polls a terminal uses.

**Found on the way: every system call paid for `spawn`'s buffers.**
`do_spawn` was inlined into `do_syscall`, so its 1.8 KB of argument
buffers were part of `do_syscall`'s frame, and every call used that
much kernel stack. `do_spawn`, `do_select` and `do_poll` are
`noinline` now, and `do_syscall`'s frame went from **1,840 bytes to
624**.

**Tests:** `apps/polltest`, 12 checks: a timeout kept, a file ready at
once and both ways, a negative descriptor ignored, a closed one
`POLLNVAL`, the limit, `select` counting and writing back its time,
`EBADF` for a closed descriptor, and a signal ending a wait with
`EINTR`. `polltest tty` has 7 more: the harness types a key only once
the program says it is waiting, for `poll` and then `select`. And
`polltest net`, in `nettest.sh`, fetches a file from the host's web
server, waiting with `poll` at every step, including end of stream.
**What the tty checks cannot prove:** that the tick's wakeup is prompt.
The 100 ms fallback alone would pass them.

### 7. Interval timers — done

`alarm` (27), `setitimer` (104) and `getitimer` (105), with all three
timers, plus `gettimeofday` (78) and `settimeofday` (79). These are the
old numbers Linux/m68k shares with i386. `clock_gettime` and the
`timer_create` family are **not** here, because I am not certain of
their m68k numbers and would rather leave a gap than guess.

**User and system time are real now.** A tick is charged to the running
task's user or system time according to what the interrupt interrupted.
The MFP stub publishes its `pt_regs` as `irq_regs` for the length of a
handler, which is what Linux's `get_irq_regs()` is. `ITIMER_VIRTUAL`
counts down only on user ticks and `ITIMER_PROF` on both.
`ITIMER_REAL` is a deadline in jiffies, checked with the other deadlines
in `task_timeouts()`. A repeating timer that falls behind is caught up
with **one** signal, not a burst. `times()` takes Linux's `struct tms`
now, and a reaped child's time goes into its parent's `cutime` and
`cstime`, as POSIX says. That changed `times()`'s signature in `ulib`,
and its seven callers now pass 0.

**One clock.** `time()`, `gettimeofday()` and file timestamps all read
`clock_get()` in `timer.c`: the RTC's second, taken **at the moment the
second changes** (the tick watches for it during the first second after
boot), plus the ticks since. So they cannot disagree, and the fraction
`gettimeofday()` reports is real. `time()` used to read the RTC directly
while the filesystem did the same separately, which was two clocks that
happened to agree. `settimeofday` and `stime` set the RTC and the base
together.

**Found on the way:** the shell reported any signal it had no name for
as "killed". `strsignal()` now lives in `string.c`, where C puts it and
where the shell's layering allows it, and both the shell and
`signal_name()` use it. An uncaught alarm reports "alarm clock" and a
double free reports "aborted".

**Tests:** `apps/timetest`, 24 checks. The clock never goes backwards
over 2,000 reads, agrees with `time()`, measures a 300 ms sleep, and
`settimeofday` moves it and puts it back. `alarm(1)` fires in about a
second and `alarm` returns what was left. A 100 ms repeating timer
fires five times in about 500 ms and `getitimer` reports it; a disarmed
timer stays off. `ITIMER_VIRTUAL` does not advance during a 500 ms
sleep and fires after 200 ms of computing, and `ITIMER_PROF` fires. The
refusals are checked. 200 ms of computing is charged to user time and
not system time, and a child's 300 ms arrive in `cutime`.
`apitest.sh` also checks that an uncaught SIGALRM ends the program and
the shell names it. The first `burn()` loop made a system call every
few microseconds and **the system-time check caught it**, which is the
check doing its job on the test's own mistake.

### 8. Pipes, `dup2`, real redirection — done

**Kernel:** `pipe` (42) and `fcntl` (55), with Linux's meanings. A pipe
is a page-sized ring with its ends as ordinary open files, so `read`,
`write`, `poll`, `dup2` and `close` need nothing special. Reading an
empty pipe with no writers is end of file. Writing one with no readers
raises SIGPIPE and fails with `EPIPE`. A write of up to `PIPE_BUF` goes
in whole or not at all. Ends are counted per open file, so a `dup`ed or
inherited end stays open until its last reference closes. `fcntl`
supports `F_DUPFD`, `F_GETFD`/`F_SETFD` (`FD_CLOEXEC`) and
`F_GETFL`/`F_SETFL`, where only `O_NONBLOCK` and `O_APPEND` can change.
`FD_CLOEXEC` is a new per-descriptor flag, not per open file, as POSIX
has it. `spawn` does not hand a close-on-exec descriptor to the child,
and a `dup` never inherits the flag.

**Process groups,** which the pipelines made necessary: ctrl-C to a
pipeline must reach every stage, and it used to go to one foreground
pid. `setpgid` (57), `getpgid` (132), `getpgrp` (65), `getppid` (64),
and the terminal's `TIOCGPGRP`/`TIOCSPGRP`. The terminal's foreground
is a **group** now. `kill` has Linux's 0, −N and −1 forms. A spawned task
joins its spawner's group, and the shell moves each job into a group of
its own. **A background task that reads the terminal is sent
SIGTTIN**, which stops it; after `fg` its read is restarted by task 5's
rules and it reads normally. the working notes said a background reader "gets
nothing". In fact nothing enforced that, and whichever task was blocked
in `read` first got the key.

**`exec_spawn` no longer gives the terminal to every child** (the task
9 note, done here). That was a shell's decision in the kernel, and it
gave a helper-spawning program's terminal to its helper. The cost is
that a ctrl-C typed during the few milliseconds of an image load goes
nowhere, which is documented in `exec.c`.

**The shell:** redirection works by pointing **its own descriptors 0–2**
at the files for the length of a command and restoring them afterwards,
as any shell does for a builtin. A program inherits them, and the old
private output descriptor is gone. `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`
and `|` all work, and the pipeline's status is the last stage's. The
builtins had to come out of `run_command` into `run_builtin()` so that
a builtin can be the first stage of a pipeline, where the shell itself
writes into the pipe after starting the readers. A builtin cannot be a
later stage, which would need the shell to be two things at once. The
shell's pipe ends are `FD_CLOEXEC`, and **the negative control shows why**:
without it every stage inherits stray write ends, no reader ever sees
end of file, and the first pipeline hangs the whole suite.

*Simplification, documented in the shell:* redirections are applied
stdout first, so `2>&1` means stdout's destination whichever order they
were written in. `sh` applies them left to right. The two differ only
for `2>&1 >file`, which nobody writes on purpose.

**Also fixed on the way:**
- **Static limits raised.** 8 descriptors per task, 16 open files in
  the whole machine, 8 tasks and 8 address spaces became 32, 128, 32
  and 32. One pipeline in a shell with a background job hit several of
  them. That is part of task 22, done early because this task needed it.
- **`isatty()` said yes to sockets.** It was built on "fstat says
  character device", which every socket, pipe end and the framebuffer
  also said. It asks `TCGETS` now, as Linux's does, and sockets report
  `S_IFSOCK`.
- **A read that already had data could still sleep.** `rw_user` hands a
  buffer over a page at a time, and a read spanning two pages asked the
  file a second time, which for a pipe or terminal that had given
  everything meant sleeping with data in hand. It now continues only if
  the file is still readable.

**Tests:** `apps/pipetest`, 27 in-process checks. They cover making a
pipe (not a terminal; a FIFO), data through it, readiness,
`PIPE_SIZE`, `EAGAIN` with `O_NONBLOCK`, a full pipe unwritable, end of
file and `POLLHUP`, SIGPIPE then `EPIPE`, every `fcntl` command,
`dup2` clearing close-on-exec, 32 descriptors, the process group the
shell made, the terminal's foreground group, `setpgid` refusals, a
helper joining its parent's group, and `kill(0, …)` reaching it.
`pipetest` also provides small tools to build pipelines from.
`apitest.sh` checks every redirection form **by reading the files back
on the host with `mtools`**, plus pipelines: program to program, builtin
to program, 100,000 bytes through three stages with every byte
checked, an unknown stage, a writer whose reader left early, ctrl-C
ending both stages of a pipeline, and a background reader stopped
rather than stealing keys that then reads its line after `fg`.

---

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

- **Task 9 must reap orphans.** A program that exits without waiting
  for its children leaves them as zombies holding task slots for ever;
  Linux reparents them to init. (The other half of this note, taking
  the terminal out of `exec_spawn`, was done in task 8.)
- **Kernel stack use is unmeasured.** `do_syscall`'s frame is 1,840
  bytes because `do_spawn` is inlined into it, against an 8 KB stack.
  It is fine today. A stack high-water mark, painting the stack and
  checking it at exit, would say how fine.

- **`mmap` of `/dev/fb0`.** Three documents give `mmap` as the reason a
  program cannot draw into the framebuffer directly. `mmap` exists now;
  what is missing is a `file_ops` hook through which a device offers
  physical pages, plus a descriptor marking for pages that are *not*
  owned, so that `vm_destroy` never hands VRAM to the page allocator.
  Not on the editor's path, so not done under task 3.

