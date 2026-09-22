# Progress

Working log for the port-enablement work: making Sage040 able to build
and run software written by other people, with a real editor as the
proof. `design.md` §11 is the reasoned list; this file is the order, and
the running state as it actually is.

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk — both can be increased
and have been.

**Status: 27 of 31 complete** -- 23 being "regression tests throughout", which is never finished; 24-31 (the standard tools, the POSIX gaps, then Python) are in progress.

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
| 8 | Pipes, `dup2`, real redirection | `>` did nothing for a program until this | **done** |
| 9 | Subprocesses: `fork`/`execve`/`waitpid` | `:!` and `:make` in an editor | **done** |
| 10 | The rest of the socket API, and the signatures | before a libc is written against the old ones | **done** |
| 11 | **VT102** emulation in `fbcon.c` | a full-screen program needs cursor addressing | **done** |
| 12 | `TIOCGWINSZ` and `SIGWINCH` | depends on 5 and 11 | **done** |
| 13 | A C library (picolibc or newlib) | the gate everything real passes through | **done** |
| 14 | VFAT long file names | 8.3 decides what can be shipped | **done** |
| 15 | `fsck`, and a clean-unmount flag | the machine cannot check its own disk | **done** |
| 16 | Build and run uEmacs | the cheapest real editor | **done** |
| 17 | Build and run vi | the other one | **done** |
| 18 | A resolver (DNS), then **NTP** | independent of the editor work; both are UDP clients and NTP wants a name | **done** |
| 19 | TCP: window scaling, timestamps, SACK, keepalives, real `TIME_WAIT` | was "deliberately not doing"; now on the list | **done** |
| 20 | Shared libraries | downstream of `mmap` and the libc | **done** |
| 21 | Paging and swapping | downstream of `mmap` | **done** |
| 22 | Interrupt-driven input **and disk**, the NVRAM, the static limits | cleanup, any time | **done** |
| 23 | Regression tests throughout | every task ships with its tests | ongoing |
| 24 | The libc and kernel gaps the standard tools need | one step shared by 25-28 | **done** |
| 25 | awk (the one true awk) | smallest, no configure: proves the build path | **done** |
| 26 | GNU sed | gnulib, a cross configure | **done** |
| 27 | GNU grep | the same, and gnulib's own regex | **done** |
| 28 | bash | the most demanding: signals, job control | |
| 29 | The small utilities: sort, wc, find, xargs, head, tail, cut, tr, uniq... | sbase (suckless): ~100 POSIX tools, MIT, one Makefile | |
| 30 | The POSIX gaps found along the way -- all of them, not only what 24-29 need | see the list under 30; Python will need most | |
| 31 | Python (CPython) | the largest port yet; 30 prepares for it | |

Tasks 16 and 17 are the point of the exercise. Everything before them is
what they need.

**Tasks 19–21 were in a "still open, deliberately" section**, justified
by the machine being small. It is not small now, so they are on the list
like everything else.

**VT102 rather than VT100** (task 11): VT102 adds insert and delete of
lines and characters (`ESC[L`, `ESC[M`, `ESC[P`, `ESC[@`), which are
exactly what an editor uses to avoid repainting. On a console where each
glyph is 128 pixels drawn one at a time, that is the difference between
usable and not. `vt102` is also already in every termcap and terminfo.

---

## Log

### 0. Groundwork — done

**RAM is 64 MB and the disk 512 MB.** Raising `-m` alone did nothing:
three limits capped what the kernel saw, all silently.

1. **`kernel.ld` put the supervisor stack at a hardcoded 4 MB**, and the
   page allocator ran from `_end` to just under it. The stack is now a
   `.kstack` section inside the image, below `_end`; the allocator runs
   to the end of real RAM.
2. **`probe_memory()` stopped at 64 MB** (`mb < 64`), so a limit looked
   like an end found. Now 2048, the emulator's cap.
3. **The `pmm` bitmap was a static array for 64 MB**, and `pmm_init()`
   clamped to it and reported the clamped figure. The bitmap is now
   *placed* (not allocated) at the front of the pool it describes, its
   own pages marked used. One page per 128 MB.

Pages the allocator got: `-m 4` 948, `64` 16,308, `256` 65,460,
`512` 130,996.

**Sizes live in `machine.conf`** (`RAM_MB`, `DISK_MB`), valid as both
make and POSIX shell; four Makefiles include it, five scripts source it.
It had been a literal `-m 4` in eleven places. The disk is a fresh
512 MB FAT16, 8 KB clusters (65,370 of them, under FAT16's 65,524);
`fsck.fat` clean.

**The working directory belongs to a task.** It was one `static struct
dir cwd` in `fs/fat16.c`, so any `chdir` moved every task. Storage is in
`struct task`, inherited by `task_create()` and `exec`; meaning stays in
the filesystem via `vfs_cwd_ino()`/`vfs_cwd_set()`/`vfs_cwd_path()`, so
`fs/` still does not include `task.h`. **Bug on the way:** task 0 is
built by hand in `task_init()`, and missing its cwd there gave every task
an empty working directory.

**`fstat`, `access`, `dup`, `dup2` added.** `fstat` is a new
`file_ops` op; every device answers `S_IFCHR`, which is what `isatty()`
was built on. `dup`/`dup2` were **already implemented** in `vfs.c` with
no system call wired to them.

*Decision:* `access(X_OK)` uses the same first-four-bytes ELF test as
`exec`, so the two can never disagree. `W_OK` is always true and says so.

#### A latent bug, found by the per-task cwd and much worse than it

**`vfs.c` stripped the leading slash off every path** (`strip_root()`),
and `path_walk()` resolves a slashless name relative to the cwd. So
`/etc/rc` from inside `/etc` meant `/etc/etc/rc`:

```
/$ cat /etc/rc          -> echo booted from /etc/rc
/etc$ cat /etc/rc       -> /etc/rc: no such file or directory
/etc$ /bin/ifconfig     -> command not found
```

`path_walk()` had handled the slash correctly all along; the fix was to
delete `strip_root()`. Unnoticed because every test ran from `/`.
`fstest.sh` and `apitest.sh` check it now.

#### Found along the way

**`>` redirection was silently broken for programs.** The shell swapped
its *own* output descriptor, which a spawned program never saw:
`hello > /OUT.TXT` printed to the terminal and left an empty file.
Deferred to task 8 (needs `dup2` on the child's table). Three documents
that claimed no redirection at all were corrected.

---

### A test suite for the ABI itself

`kernel/apitest.sh` tests the **system call surface**, where the other
suites test subsystems. Checks are made by **programs** in `apps/`, each
printing `ok`/`FAIL` per line, counted and named by the script — a new
check is a line of C. A group is added only once its calls work, so a
failure there is always a regression.

**747 checks across twelve suites now:** 12 device programs, 59 fs, 368
api, 29 edit, 18 vm, 17 net, 95 vt, 90 libc, 23 fsck, 9 uemacs, 9 vi,
18 dns. The libc, uEmacs and vi suites need `make libc` first.

### 1. Grow the user address space — done

**2 MB → 256 MB.** `USER_VA_BASE` `0x10000000`, `USER_VA_END` now
`0x20000000`. The stack is at the top and grew from 64 KB to **1 MB**,
leaving ~255 MB between image and stack for `brk` and `mmap`.

**The limit was the bookkeeping, not the number:** root, pointer and
eight 256-byte page tables were packed into one physical page. 256 MB
needs up to 1024 page tables, so **tables are allocated on demand**: a
table page's first 512-byte slot links to the previous table page (a
freeable list), the other seven slots hold tables.

**`as_pagetable()` gained a `create` flag, and the flag is the whole
difference between mapping and translating** — a translate that could
create tables would let a wild pointer allocate memory.
`vm_mapped_pages()` and `vm_destroy()` skip a whole table when it is
absent: 1024 iterations instead of 65536, and `free` calls it every
prompt.

Measured: a program costs 262 pages (1 MB stack plus image and tables);
`ps` and `free` agree; all 130 existing checks pass.

*Decision:* the stack is 1 MB and **eagerly mapped** — no demand paging
until task 21. 8 MB worst case across eight tasks; removes stack overflow
as something ported software trips over.

#### A leak found on the way

**Killing a background job did not free its memory.** `task_reap()` ran
only from `waitpid`, which the shell only called from `jobs` and after
`fg`. With a 256 MB space that is a megabyte a time against `TASK_MAX`
8. **The shell reaps at every prompt now** (revisit with `SIGCHLD`,
task 5). `vmtest.sh` checks it: used pages 37 → 299 → 37. **Negative
control:** with the reap removed it fails (299). 133 checks.

#### The session that stopped, and why

Every shell command began returning 1 with no output. Not `exec`:
**`/tmp` is a tmpfs with a per-user quota, and ~5.6 GB of old 512 MB disk
images under a directory in `/tmp` had filled it**, so command output had
nowhere to go. `df` showed 1.6 GB free because a quota is not a full
filesystem. `/tmp/sage040-cleanup.sh` clears it. **Keep 512 MB disk
copies in `scratch/`, on `/home`, never under `/tmp`.**

### 2. `brk`/`sbrk` — done

`__NR_brk` is Linux's **45**, with Linux's convention: returns the new
break on success, the **old** one on failure (never an errno); `brk(0)`
asks. Every Linux `malloc` detects failure by comparing.

- The break belongs to the address space (`brk_start`, `brk_cur` in
  `struct addrspace`). `exec` sets it to the page after the highest
  `PT_LOAD`, page aligned, so a shrink can never unmap the program. It
  may grow to one guard page below the stack (`USER_BRK_LIMIT`).
- `vm_unmap()` is new, and flushes **before** the page can be reused.
- `lib/`: `brk()` returns 0 or `-ENOMEM`, `sbrk()` the old break or
  `(void *)-1`, the break cached as glibc does. `ulib.h` now includes
  `errno.h` (pure macros, allowed by the layering check). `sysinfo()`
  wrapper added.

*Decision:* a request the machine plainly cannot meet is **refused up
front**. The first version mapped until the allocator ran dry and rolled
back, but the page tables built on the way stayed until the address
space died — ~37 pages lost per failed `sbrk`. The rollback remains as a
net; nothing can reach it today (non-preemptible kernel).

**Growth refuses to pass over an already-mapped page**, so the heap can
never silently replace an `mmap`.

**Tests:** `apps/memtest` 26 checks plus 3 in `apitest.sh`: heap start,
growth, zeroed pages, unaligned break, shrink returning pages, regrown
pages being new, refusals leaving the break alone, an over-size request
returning *every* page, an 8 MB heap, touching a page given back and
dying. **Negative control:** without the up-front refusal, "every page
returned" fails.

### 3. `mmap`/`munmap`/`mprotect` — done

**The ABI is Linux/m68k's, shapes as well as numbers.** *Correction:* the
design note planned to depart from Linux at 90 because six arguments do
not fit `d1`–`d5`. Wrong — Linux/m68k passes a sixth in **`a0`**, and 90
is `old_mmap`. So: `mmap2` (192), six registers, offset in pages;
`old_mmap` (90), a `struct mmap_arg_struct` pointer, offset in bytes;
`munmap` 91; `mprotect` 125. The trap gate now passes `a0` to every
call. That glibc's m68k `sysdep.h` uses `a0` this way is **from memory,
not verified**.

**There is no table of mappings** (the note planned a 32-entry table
with splitting). The page tables already record ownership and
protection per page, so partial `munmap`/`mprotect` need no splitting.
Cost: nothing can say which `mmap()` a page came from; nothing asks.

**`PROT_NONE`** is an invalid descriptor (type 00) that keeps the
physical address and sets software bit `DESC_SW_NONE` (bit 11). It
faults like an unmapped page but `vm_destroy`/`vm_mapped_pages`/
`vm_unmap` treat it as owned; contents survive the round trip.

**Placement is top down from below the stack's guard page**, never below
the break (Linux's layout). A free hint is honoured; `MAP_FIXED`
replaces; `MAP_FIXED_NOREPLACE` refuses with `EEXIST`.

*Decision, reversing the note:* **`MAP_SHARED` on a file is allowed
read-only**, `-ENODEV` only with `PROT_WRITE`. File mappings are copies
made at `mmap` time; a writable shared copy would silently not write the
file, a read-only one merely misses later writes. Anonymous `MAP_SHARED`
is private in effect — exact until `fork` (task 9 must revisit).

**`PROT_EXEC` means nothing: the 68040 page descriptor has no execute
bit.** (This corrects the task 5 note.)

**Tests:** `memtest` 77 checks, `apitest.sh` 107 in all: anonymous and
file mappings, partial `munmap`, `mprotect` both ways incl. `PROT_NONE`
preserving contents, `EFAULT` for `read()` into read-only and `write()`
from `PROT_NONE`, hints, both `MAP_FIXED` forms, heap refusing to grow
over a mapping, every refusal's errno, `old_mmap`, a file mapping
outliving its descriptor, file position untouched. Three deliberate
faults from the shell (unmapped, read-only write, `PROT_NONE` read).
`free` 37 pages before and after a `memtest` that exits with mappings in
place.

**Negative control:** with `pflusha` removed from `vm_protect()`, the
read-only write goes through and three checks fail.

### 4. `malloc` in `lib/` — done

`lib/malloc.c`: `malloc`, `free`, `calloc`, `realloc`, `malloc_check()`,
glibc's `mallinfo()`. **Written to be thrown away at task 13**; it exists
so tasks 5–12 need not wait. More than the planned single first-fit list:

- **Boundary tags** (dlmalloc style): 4-byte header, footer only when
  free, "previous in use" bit in the next header. O(1) merges.
- **Segregated free lists** per power of two, first fit within a list.
- **≥ 128 KB from `mmap`**, returned by `munmap` on `free` (as glibc).
- **Top of heap trimmed** by negative `sbrk` once > 256 KB is free.
- **`realloc` grows in place** into a free neighbour or, for the last
  block, by moving the break (an editor's gap buffer needs this).
- **Double free or foreign pointer** is reported on stderr and exits 134
  (shell reads `SIGABRT`).
- **Segments start 8-byte aligned** even if the program left the break
  odd. Found on review, not by a test.

**Programs are linked with `--gc-sections`**; every one shrank ~3 KB.

**Bug:** `lib/user.ld` still said `LENGTH = 2M - 64K` (missed in task 1),
refusing any program over ~1.9 MB. Now `256M - 1M - 4K`.

**Tests:** `apps/malloctest`, 27 checks: alignment, `malloc(0)`,
`free(NULL)`, `calloc` zeroing dirty memory and overflow, impossible
request → NULL, `realloc` contents and in-place growth, the `mmap` path,
trimming; then 40,000 random operations over 512 slots with per-slot
patterns and `malloc_check()` every 500, ending with nothing in use.
`apitest.sh` checks the double free, and **waits for the workload rather
than sleeping past it** (duration is host dependent). **Negative
control:** without backward merging in `coalesce()`, `malloc_check()`
reports adjacent free blocks and six checks fail.

### 5. Signals — done

#### Found first: the system call gate read `a6` as the status register

`_trap0_entry` saved `d1`–`a6` with a comment "13 registers, 52 bytes"
and read the SR from `56(%sp)`. **It is fourteen registers, 56 bytes**,
so it read the saved `a6`. Every shell call arrived with "SR" 0 or 7,
never `0x2000`. **So every kernel-task system call was taken as from
user mode**: signals delivered and preemption possible on exit —
contradicting the working notes and the kernel README. Hidden because the
shell ignores SIGINT/SIGTSTP and SIGCHLD defaults to ignored.

**Fix:** every kernel entry saves all fifteen registers, `d0` included,
in the layout the MFP stub and exception path already used, as `struct
pt_regs` (`ptregs.h`). The result goes in the saved `d0`;
`task_ret_to_user()` reads the real SR from `pt_regs`.

**That exposed the policy the bug had been standing in for** (both
Linux's):

- **A kernel task takes no signals.** `signal_send` drops them; `kill`
  on one returns `EPERM`.
- **An ignored signal is discarded when sent** (explicitly ignored or
  default-ignored like SIGCHLD). A blocked one is kept.

**Tests:** `sigtest` checks a hand-written `trap #0` preserves `d1`–`d7`
and `a0`–`a6` with the result in `d0`; `apitest.sh` checks `kill 2` and
`kill -9 2` are refused. **Neither would have failed on the old gate. The
bug has no lasting regression test** — its only symptom, a kernel task
preempted at syscall exit, is unobservable from outside. Found and
confirmed by measurement.

#### Found second: nothing saves the FPU across a context switch

No kernel code touched the FPU, so tasks shared FP registers and
rounding mode (unseen: the cube was the only FP program).

**Fixed:** a 208-byte FPU area per task; `schedule()` does `fsave`, then
`fmovem` of `fp0`–`fp7` and the control registers unless the frame is
null, and the reverse. **QEMU's `fsave` always writes an idle frame and
its `frestore` does nothing**, so a new task starts with a fabricated
idle frame and zeroed registers; a null frame would inherit the previous
task's registers under QEMU.

**Test:** `apps/fptest N` fills the FP registers and `fpcr` from N,
yields and spins for three seconds, comparing raw bits. `apitest.sh`
runs two at once. The first version ran fixed rounds and **finished
before the second started, passing without testing anything**; now
timed by the clock. **Negative control:** without the save both report
lost state.

#### Handlers

**ABI: Linux/m68k's old signal interface.** 31 signals; `sigaction` (67,
m68k field order: handler, mask, flags, restorer), `sigprocmask` (126),
`sigpending` (73), `sigsuspend` (72, one argument), `pause` (29),
`sigreturn` (119); 32-bit masks, signal N in bit N−1 (the kernel's
`SIGMASK()` changed to match). No `rt_` calls. Whether m68k's
`sigsuspend` takes one argument or three is **not verified**.

**Frame:** return address (the restorer), signal number, code 0,
context pointer, then `struct sigcontext` with every register, old mask,
USP, sr, pc and full FPU state. The handler returns to
`__sigreturn_trampoline` in `crt0.s`. **Only the saved sr's condition
codes are honoured**, which stops a forged frame reaching supervisor
mode. `sa_restorer` is **required**; the library always supplies it.

*Correction to the planning note:* it justified the trampoline by "this
MMU can mark the stack non-executable". The 68040 has no execute bit.
The trampoline stays because Linux uses `sa_restorer`, stack code needs
cache pushes on a real 68040, and source is readable.

**Restart:** blocking calls return `-EINTR`; `pause`/`sigsuspend` an
internal `-ERESTARTNOHAND`. If no handler ran, the call restarts
invisibly (number back in d0, pc back 2 bytes); after a handler only
with `SA_RESTART`, never for `pause`/`sigsuspend`. So a `read` stopped
by ctrl-Z carries on after `fg`. `current->syscall_nr` marks that a call
is in progress.

**Also fixed or added:** SIGKILL resumes a stopped task (it used to sit
pending for ever); SIGSTOP/SIGTTIN/SIGTTOU stop; SIGCONT resumes when
sent, even if caught or ignored; child exit sends SIGCHLD (only stops
did); `SA_NOCLDSTOP`, `SA_NODEFER`, `SA_RESETHAND`; `kill(pid, 0)`.
`ulib`: `sigaction`, `signal` (with `SA_RESTART`, as glibc), mask calls,
`pause`, `kill`, `raise`, `getpid`, `waitpid`, `spawn`, `sigset`
helpers.

**Refused, not half done:** `SA_SIGINFO`, `SA_ONSTACK` (`EINVAL`). **A
fault's own signal cannot be caught** — the format-7 frame re-runs the
access on `rte`; faults still end the program.

#### Two more bugs found by the tests, both older than this task

**A signal to one sleeper woke every task in `nanosleep`**: all share
one queue and `signal_send` used `wake_all(t->queue)`; the others
returned 0 with their sleep cut short. `wake_signalled()` in `wait.c`
now wakes exactly one.

**A program that spawned another made every task read the wrong
memory.** `exec_spawn` saved and restored `uaccess_current()`, which for
a program is its own address space, not "no override" — so the restore
installed the spawner's space as a global override. Every task's calls,
the shell's included, then used the spawner's memory: the child printed
the parent's data and its `nanosleep` read a twelve-hour time off the
parent's stack. It healed at the shell's next spawn. Bisected: present
at the task 0 commit. `uaccess_set()` now returns the raw previous
setting, which is restored.

**Also found (deferred to task 9):** `exec_spawn` gave the terminal to
every child, a shell decision in the kernel; and **nothing reaps an
orphan**, which holds a task slot for ever.

**Tests:** `apps/sigtest`, 32 checks: handlers, mask during a handler
and with `SA_NODEFER`, `sa_mask`, `SA_RESETHAND`, block/pending/unblock,
ignore discarding a pending signal, refusals, `kill(pid, 0)`, every
integer and FP register (and `fpcr`) surviving a handler that wrecks
them; `pause`, `SA_RESTART` on a real `nanosleep` (EINTR at 200 ms
without, full 1 s with), `sigsuspend` restoring its mask, SIGCHLD on
exit. `apitest.sh` adds: a second sleeper sleeping its full time, ctrl-C
to a handler in `pause` and in a syscall-free loop, an unwritable frame
ending the program, a forged supervisor `sigreturn` ending in a
privilege violation, SIGSTOP then SIGKILL on a stopped job.

**Negative control on the forgery check:** honouring the whole sr, the
forgery ran in supervisor mode and took the machine down — and **the
first version of the check passed anyway**, because it only looked for
the absence of a message the crash prevented. It now requires the
privilege violation.

**The harness trap bit again:** a pre-built ctrl-C arrived during an
earlier program. Timed keys now go through `send_after`, which waits for
the program to say it is ready.

### 6. `select`/`poll` — done

`poll` (168), `_newselect` (142), old `select` (82, a `struct
sel_arg_struct` pointer, as on Linux/m68k). `fd_set` is 1024 bits.
`select` writes back the unused time. A signal gives `-EINTR` after a
handler or a restart if none ran; the restart **begins the timeout
again** (no restart block; Linux would resume).

**Readiness is a new `file_ops->poll`**, falling back to `FIONREAD`
(readable when bytes wait, always writable), which covers terminal,
serial and keyboard with no new code. No `FIONREAD` means a regular
file: always ready. Only sockets needed their own (`sock_poll` over a
new `tcp_poll`): a listener is readable when `accept` would not wait; a
connection also at end of stream or reset.

**Waiting differs from the design note** (task on several queues): **one
shared queue, woken by the terminal from the tick**, plus a short
timeout of its own — 100 ms, or 20 ms with a socket watched, because the
network stack only does protocol work when asked.

*Limit, documented in `uapi.h`:* a **canonical** terminal is readable
when a character waits, not a whole line (the line is assembled inside
`read()`). Raw mode is exact.

**Found on the way: every system call paid for `spawn`'s buffers.**
`do_spawn` was inlined into `do_syscall`, putting 1.8 KB of argument
buffers in its frame. `do_spawn`, `do_select`, `do_poll` are `noinline`;
`do_syscall`'s frame went **1,840 → 624 bytes**.

**Tests:** `apps/polltest` 12 checks (timeout, file ready both ways,
negative fd ignored, closed fd `POLLNVAL`, the limit, `select` count and
time write-back, `EBADF`, `EINTR`); `polltest tty` 7 more with keys typed
once the program says it waits; `polltest net` in `nettest.sh` fetches
from the host web server using `poll` at every step incl. end of stream.
**Not proved:** that the tick's wakeup is prompt — the 100 ms fallback
alone would pass.

### 7. Interval timers — done

`alarm` (27), `setitimer` (104), `getitimer` (105), all three timers;
`gettimeofday` (78), `settimeofday` (79) — the old numbers m68k shares
with i386. **`clock_gettime` and `timer_create` are not here**: their
m68k numbers are uncertain, and a gap beats a guess.

**User and system time are real.** Each tick is charged by what it
interrupted; the MFP stub publishes its `pt_regs` as `irq_regs` (Linux's
`get_irq_regs()`). `ITIMER_VIRTUAL` counts user ticks, `ITIMER_PROF`
both; `ITIMER_REAL` is a jiffies deadline in `task_timeouts()`. A
repeating timer that falls behind catches up with **one** signal.
`times()` takes Linux's `struct tms`; a reaped child's time goes to the
parent's `cutime`/`cstime`. `ulib`'s `times()` signature changed; seven
callers now pass 0.

**One clock.** `time()`, `gettimeofday()` and file timestamps all read
`clock_get()` in `timer.c`: the RTC second **captured at the moment it
changes** (watched during the first second after boot) plus ticks since.
Previously `time()` and the filesystem each read the RTC — two clocks
that happened to agree. `settimeofday` and `stime` set RTC and base
together.

**Found on the way:** the shell called any unnamed signal "killed".
`strsignal()` is now in `string.c` (allowed by the layering) and used by
the shell and `signal_name()`: "alarm clock", "aborted".

**Tests:** `apps/timetest`, 24 checks: monotonic over 2,000 reads,
agreement with `time()`, a 300 ms sleep measured, `settimeofday` round
trip, `alarm` firing and returning the remainder, a 100 ms repeating
timer firing five times in ~500 ms, disarm, `ITIMER_VIRTUAL` idle during
sleep and firing after 200 ms of work, `ITIMER_PROF`, refusals, user vs
system charging, a child's 300 ms in `cutime`. `apitest.sh`: uncaught
SIGALRM named by the shell. The first `burn()` loop made frequent system
calls and **the system-time check caught it** — the check catching the
test's own mistake.

### 8. Pipes, `dup2`, real redirection — done

**Kernel:** `pipe` (42), `fcntl` (55).

- A pipe is a page-sized ring whose ends are ordinary open files.
  Empty with no writers is EOF; no readers raises SIGPIPE and `EPIPE`;
  writes up to `PIPE_BUF` are atomic. Ends are counted per open file.
- `fcntl`: `F_DUPFD`, `F_GETFD`/`F_SETFD`, `F_GETFL`/`F_SETFL` (only
  `O_NONBLOCK`, `O_APPEND` change). `FD_CLOEXEC` is per descriptor, as
  POSIX; `spawn` withholds it from the child, `dup` never inherits it.

**Process groups** (ctrl-C must reach every pipeline stage): `setpgid`
(57), `getpgid` (132), `getpgrp` (65), `getppid` (64),
`TIOCGPGRP`/`TIOCSPGRP`. The terminal's foreground is a **group**;
`kill` has the 0, −N and −1 forms; a spawned task joins its spawner's
group and the shell gives each job its own. **A background reader gets
SIGTTIN** and stops; after `fg` its read restarts. *Correction:*
the working notes said a background reader "gets nothing" — nothing enforced
that, and whichever task blocked in `read` first got the key.

**`exec_spawn` no longer gives the terminal to every child** (the task 9
note, done here). Cost: ctrl-C during the few ms of an image load goes
nowhere; documented in `exec.c`.

**The shell** points **its own descriptors 0–2** at the files for the
length of a command and restores them; the private output descriptor is
gone. `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`, `|` work; status is the last
stage's. Builtins moved to `run_builtin()` so one can be the *first*
stage (not a later one). Shell pipe ends are `FD_CLOEXEC`. **Negative
control:** without it stages inherit stray write ends, no reader sees
EOF, and the first pipeline hangs the suite.

*Simplification, documented in the shell:* redirections apply stdout
first, so `2>&1` means stdout's destination in either order; `sh` goes
left to right. Differs only for `2>&1 >file`.

**Also fixed on the way:**
- **Static limits raised** (part of task 22, done early): descriptors
  per task 8 → 32, open files 16 → 128, tasks 8 → 32, address spaces
  8 → 32. One pipeline plus a background job hit several.
- **`isatty()` said yes to sockets** (and pipes, framebuffer) because it
  trusted `S_IFCHR`. It asks `TCGETS` now, as Linux; sockets report
  `S_IFSOCK`.
- **A read with data in hand could still sleep:** `rw_user` works a page
  at a time and asked the file again for the second page. It now
  continues only if the file is still readable.

**Tests:** `apps/pipetest`, 27 checks: pipe creation (not a tty; a
FIFO), data, readiness, `PIPE_SIZE`, `EAGAIN` under `O_NONBLOCK`, full
pipe unwritable, EOF and `POLLHUP`, SIGPIPE then `EPIPE`, every `fcntl`
command, `dup2` clearing close-on-exec, 32 descriptors, process and
foreground groups, `setpgid` refusals, helper joining the group,
`kill(0, …)`. `apitest.sh` checks every redirection form **by reading
the files back on the host with `mtools`**, plus pipelines: program to
program, builtin to program, 100,000 bytes through three stages checked
byte for byte, an unknown stage, a reader leaving early, ctrl-C ending
both stages, and a background reader stopped and then reading after `fg`.

---

### 9. Subprocesses: `fork`/`execve`/`waitpid`, and `/bin/sh` — done

**`fork` (2)** copies the address space eagerly (`vm_clone`): every
owned page, with its protection, `PROT_NONE` included, refused up front
if the memory is not there. Copy-on-write waits for task 21's fault
handling. The child's kernel stack is `build_stack`'s usual `pt_regs` +
format 0 frame with the parent's registers copied in and d0 = 0, so its
first `rte` lands after the parent's trap. It inherits descriptors (all,
with their `FD_CLOEXEC`), cwd, handlers, mask, group and FPU state. It
does not inherit pending signals, timers or times.

**`execve` (11)** builds the new image completely before touching the
old one, so a failure returns the error into an intact caller. Then:
the old address space goes, `FD_CLOEXEC` descriptors close, caught
signals reset to default (ignored ones stay), the FPU is fresh, and the
registers are cleared. **Arguments are copied into an 8-page block from
the page allocator.** The old stack arrays (8 × 64 bytes) were too small
for `sh -c "…"` and could not grow on an 8 KB stack. Limits: 256
arguments, 256 environment strings, 32 KB total. `spawn` uses the same
block.

**`waitpid` speaks Linux's status encoding** (`W*` macros in `uapi.h`)
and takes `WNOHANG`, `WUNTRACED` and `WCONTINUED`, and pid −1, 0 and
−N. The shell converts back to its `128 + signal` convention for `$?`.
**Two old bugs:** `task_wait` never checked for signals, so `waitpid`
was uninterruptible; and it reported every stop as SIGTSTP, whatever
stopped the child.

**Orphans are reaped.** A task that exits orphans its children, and the
scheduler reaps a parentless zombie. There is no init to do it, and a
task cannot free the stack it is standing on.

**`/bin/sh` is the kernel's own `shell.c` and `edit.c`, built as a
program** (`system/sh.c`, 39 KB). That works because the layering rule
already confined them to system calls. `shell_main()` takes the
environment it was given and runs `-c CMD`, `FILE`, or interactively
until end of input or `exit N`. This is what `:!`, `:make` and
`system()` exec. To get there, the system call wrappers moved from
`syscall.c` into `sysuser.c` (compiled into both), and `syscall.h`,
`string.h`, `edit.h` and `errno.c` include `types.h` rather than
`kernel.h`, which drags in the hardware header.

**The shell learned quoting**, which `sh -c` made essential: `'…'`
literal, `"…"` with `$` still expanded, `\` quoting the next character.
It is honoured in expansion, at the `|` cut and in word splitting, and
a quoted `>` is an argument. Not-found is now status 127 and
not-executable 126, as in sh.

**Found on the way: the boot ROM loaded at most 128 KB**, on the
filesystem path too, although its README said that path had no limit.
The kernel crossed 128 KB with this task and simply stopped booting
("short read"). The limit is now 960 KB, the most the raw fallback's
gap allows. Also: `exec.h` declared `exec_spawn` twice, one copy under a
comment from before there were tasks.

**Tests:** `apps/proctest`, 23 checks: `fork` and private memory, the
heap copied, a pipe across fork, a shared file position, every `W*`
status (exit, kill, stop, continue), `WNOHANG`, `-1`, `ECHILD`, `EINTR`,
`execve` with arguments and environment, a failed `execve`,
`FD_CLOEXEC` honoured (a reader sees EOF at once), handlers reset and
ignores kept, `system()` through `/bin/sh` including a pipeline and
127, and an orphan not leaking. `apitest.sh` adds `sh -c`, `sh FILE`,
an interactive `sh` with `exit 4` reaching `$?`, and the quoting
cases. **Negative control:** with orphan reaping off, the orphan check
fails.

### 10. The socket API, loopback, and `netd` — done

**The whole Linux/i386 socket range, 359–373, with Linux's
signatures:** `struct sockaddr` plus `socklen_t`, `struct in_addr`, and
the flags arguments. That means `socketpair`, `accept4` (364 is
`accept4` on i386; `accept` is the library calling it with no flags),
`get`/`setsockopt`, `getsockname`/`getpeername`, and
`sendmsg`/`recvmsg` (scatter/gather, no ancillary data). `ulib` has
`send`/`recv` as wrappers, as glibc does, and POSIX's `inet_addr`,
`inet_aton` (two arguments) and `inet_ntoa`. Every program using
sockets was updated.

**Behaviour:** blocking calls wait until satisfied. The old **30-second
`ETIMEDOUT`** on every read is gone, and timeouts are `SO_RCVTIMEO` /
`SO_SNDTIMEO` → `EAGAIN`. `O_NONBLOCK`, `MSG_DONTWAIT` and `FIONBIO`
work. A non-blocking `connect` returns `EINPROGRESS`, `poll` reports
POLLOUT on completion, and `SO_ERROR` says how it ended. A closed
stream raises SIGPIPE unless `MSG_NOSIGNAL`. `bind` accepts only
`INADDR_ANY`, loopback or the machine's own address; port 0 picks a
free one; `SO_REUSEADDR` takes a port back from `TIME_WAIT`; `listen`
binds an unbound socket. UDP has a four-datagram queue (it was one
slot) and `MSG_TRUNC`/`MSG_PEEK`. `AF_UNIX` exists as `socketpair`
only, stream only: a pipe each way, in `pipe.c`.

**Loopback** (the user asked for it mid-task): `127.0.0.0/8` and the
machine's own address go onto a loopback queue in `ip_output` and are
delivered by `net_poll`, **with or without a configured interface**.
`ping 127.0.0.1` works with no network.

**`netd`**, a kernel task, runs the protocol fifty times a second
whether or not any program is in a socket call. Before it, an idle
connection nobody was reading ACKed nothing and made its peer
retransmit. Being a kernel task, and so never preempted, it keeps the
stack lock-free. It also lets **`close` stop blocking**: the
connection is released (`tcp_release`) as an orphan that finishes its
FIN handshake and `TIME_WAIT` on its own. `close` used to linger for
half a second and then free the connection whatever its state.

**Bugs found, all older than this task:**
- **TCP's FIN took a byte's sequence number.** `close` sent the FIN at
  once, at `snd_nxt`, with data still queued behind a full window, and
  `send_data` refused to send at all after close. So the queued data
  was never sent, and the FIN took a sequence number the data needed.
  Found as 100 KB over loopback arriving **one byte short**. The FIN
  now waits (`fin_pending`) until the data is out, and a retransmission
  resends data and then the FIN. The host-side test never caught it,
  because there the guest was never the sender that closed.
- **`sendto`/`recvfrom` moved at most 512 bytes** (a stack buffer), so a
  larger datagram was sent truncated and reported as fully sent.
- **`udp_output` refused datagrams over 1024 bytes**; the real limit is
  1472.
- **IP allowed a frame 14 bytes over Ethernet's maximum**: the payload
  limit forgot the Ethernet header.
- **UDP checksums were verified against the interface's address**, not
  the datagram's destination: wrong for broadcasts (hence an exception
  for DHCP) and for loopback.
- **A retransmission timeout was reported as "connection refused".**
  The give-up path set `reset` as well; `timed_out` now tells them
  apart.
- `tcp_close` frees a connection outright in some states, so
  `shutdown` on one still connecting would have left a dangling pointer.

**Limits raised:** 32 sockets (was 8), 16 TCP connections (was 8), 64
pipe rings.

**Tests:** `apps/socktest`, 53 checks, all over loopback and pairs, so it needs no
host: pairs both ways, `MSG_PEEK`/`MSG_DONTWAIT`, half-close, `EPIPE`
with and without SIGPIPE, parent and child over a pair; TCP bind,
listen, non-blocking connect and accept, `SO_ERROR`, peer and socket
names, `FIONREAD`, `SO_RCVTIMEO`, half-close, 100 KB from a child,
refused connections both ways, port 0, `EADDRINUSE` from a real
`TIME_WAIT` and `SO_REUSEADDR`, `EADDRNOTAVAIL`; UDP boundaries,
sender address, `MSG_TRUNC`, the 1472 limit, connected UDP,
`sendmsg`/`recvmsg`; the options and refusals. Plus `ping 127.0.0.1`
from `apitest.sh`. The 100 KB check is the negative control for the
FIN fix: it failed before, by exactly one byte.

### 11. VT102 in `fbcon.c` — done

`fbcon.c` was CR, BS, TAB, LF and three escapes; it is a VT102 now:
CUP/HVP, relative moves, CHA/VPA, save and restore, IND/NEL/RI; ED, EL
and ECH; IL, DL, ICH, DCH and insert mode; **scroll regions** and origin
mode; SGR bold, underline, reverse and the ANSI colours; DEC graphics
through G0/G1 and SO/SI; tab stops; DECAWM, DECTCEM, LNM, RIS, DECALN;
DSR and DA replies. A DEC-style parser: C0 controls inside a sequence
are acted on, CAN and SUB abandon it. Details in `design.md`, "The
console".

**Behaviour that changed:**

- **LF is a bare line feed.** `ONLCR` in `tty.c` already supplied the
  CR for everything written through `/dev/console`; only a program
  writing `/dev/fbcon` directly sees the difference, and none did.
- **The last column wraps late** (`xn`), so a full bottom line no longer
  scrolls. Backspace from that state moves off the last column.
- **Tab only moves.** It used to write spaces.
- The console's colours are palette entries 16-31, leaving 0-15 to
  drawing programs; default green is unchanged.

**The blitter learnt to move down and right.** `sm501_copy` refused
those (`-ENOSYS`) because nothing needed them; insert-line and
insert-character do. Right-to-left is bit 27 of the 2D control word,
and its coordinates are the BOTTOM-RIGHT corner of each rectangle --
read from QEMU's `sm501_2d_operation`.

**Replies only when the screen is the only output.** With the serial
line enabled a host terminal answers `ESC[6n` too, and two answers
corrupt the asking program's input. Replies arrive through an input
source named `fbcon`, so the boot banner now says "input from ttyS0
fbcon kbd0" (`fstest.sh` updated).

**New interfaces:** `/dev/vcsa` (Linux's format: rows, columns, cursor
column and row, then character and attribute per cell),
`FBCON_REDRAW` on `/dev/fbcon`, and `tcgetattr`/`tcsetattr` in `lib/`.

**Tests:** `kernel/vttest.sh`, a new suite. `apps/vtcheck` makes 59
checks through `/dev/vcsa`. Then `vtcheck blit` does every
blitter-moving operation, the harness screenshots, the console redraws
from its buffer, and it screenshots again; the two must be identical.
**Negative control:** a forward copy in place of right-to-left passes
all 59 vcsa checks and fails the screenshot comparison by 7,615
pixels -- which is why the suite has both halves.

The one failure on the first run was the test's own expectation for
insert mode (`XYZC` where a VT102 gives `XYZBC`).

### 12. `TIOCGWINSZ` and `SIGWINCH` — done

Linux's numbers (0x5413/0x5414) and `struct winsize`, in the syscall
layer's ioctl table so user pointers are checked.

**The answer is the smallest enabled output.** Two outputs of different
sizes are live at once, and a program told the screen's 30 rows while an
80x24 terminal is also showing its output paints rows that terminal
lacks. The screen reports its own size (`fbcon.c` answers `TIOCGWINSZ`,
so `tty.c` still does not know there is a screen); the serial line
cannot, so it is 24x80 until `TIOCSWINSZ`. That makes `TIOCSWINSZ` the
**line's** size rather than the answer. Setting the answer directly
was rejected: after `resize` on a 50-row xterm every program would
paint 50 rows onto a 30-row screen.

**`SIGWINCH`** goes to the foreground group whenever the answer
changes -- from `TIOCSWINSZ`, or from `console` switching an output on
or off -- and not when it does not. Default action ignore (it already
was).

**New programs:** `stty` (size, `rows`/`cols`, the honoured flags,
`raw`/`sane`) and `resize`, which asks the terminal (cursor to 999;999,
`ESC[6n`) as xterm's resize(1) does. `TERM=vt102` in the shell's
environment.

**Tests:** in `vttest.sh`, now 95 checks. `apps/winsize` makes 21:
the boot size, the screen alone with its pixel size, each way the
answer can change and the SIGWINCH each sends, no signal when nothing
changed, the screen limiting a bigger line, a background group not
signalled, default action, `EFAULT`. The harness plays a 40x100
terminal answering `resize`, then checks `stty size` with the screen on
(30 80), off (40 100) and after `stty rows 20 cols 60`. **Negative
control:** with the `signal_group` call removed, exactly the five
SIGWINCH checks fail.

**Named `winsize`, not `winchtest`:** nine characters, and FAT's 8.3
truncated it to a name the shell could not find.

### 13. A C library: picolibc — done

**picolibc 1.8.12**, in `libc/` (`make libc`; installs to
`~/m68k/sage040-libc`). Chosen over newlib because picolibc already has
`libos/linux`: a POSIX layer over Linux's system calls, with arm,
aarch64 and x86 backends, that translates picolibc's newlib-flavoured
errno and signal numbers to and from Linux's. This kernel's ABI is
Linux/m68k's, so the port is:

- **An m68k backend** (`libc/picolibc/libos/linux/machine/m68k/`): the
  `linux-*.h` constants, taken from the i686 set and corrected against
  Linux's own m68k sources (syscall table, `O_DIRECTORY` and friends,
  `POLLWRNORM`/`POLLWRBAND`, no `SA_RESTORER`); `syscall()` in assembly;
  and `pause`, `usleep`, `select`, which picolibc's Linux layer lacks on
  every architecture. New files only.
- **One patch** to picolibc (`libc/patches/`): an `SA_SIGINFO` handler
  got Linux's number in `si_signo`. Not m68k-specific.
- **`crt0.s`, `sage040.ld`, `libc.mk`** for programs.
- **The Linux calls picolibc makes that the kernel lacked**, in a new
  `kernel/syslinux.c`: `statx`, `getdents64` (on real directory
  descriptors -- `open` of a directory now works), the `*at` calls,
  `pipe2`, `dup3`, `wait4`, `clock_gettime`, `_llseek`, `prlimit64` and
  the rlimits, `getrandom`, `umask`, the uid calls (one user, root),
  `vfork`/`clone` as fork, no-op `mlock`/`madvise`/`msync`, `lstat`,
  `readlink`, `chmod`; `TCGETS2`/`TCSETS2` in the tty; and the `rt_sig*`
  family with **`SA_SIGINFO`**, Linux/m68k's exact rt frame with
  `siginfo` and `ucontext` (a handler that edits the context changes
  what resumes). Handlers without `SA_RESTORER` now return through a
  trampoline the kernel writes into the frame, as Linux/m68k's do.
  Structure sizes are pinned with `_Static_assert`.

FAT has no inode numbers, and some programs need them nonzero and
consistent between `stat` and `readdir`: a directory is its first
cluster, a file is its parent's cluster above its slot
(`ino_for()` in `fs/fat16.c`).

#### Bugs found on the way, all of which had passed every test

1. **The socket calls had i386's numbers**, all fifteen three too high:
   `socket` was where Linux/m68k has `connect`. Kernel and programs
   shared the header, so they agreed perfectly. `sync` was 166
   (`getpagesize`); the private `spawn`/`jobctl`/`netctl` were 400-402,
   which are `msgsnd`/`msgrcv`/`msgctl`, and are now 1000-1002.
   **`kernel/abicheck.sh`** now checks every `__NR_` in `uapi.h` against
   Linux's m68k table (`kernel/linux-m68k-syscalls.txt`) before each
   link; with `socket` put back at 359 it fails the build.
2. **Programs did not depend on `uapi.h`**, so renumbering left every
   built program calling the old numbers -- apitest's first run after
   the renumbering failed 52 checks. `lib/program.mk` now lists
   `uapi.h` and `types.h`; the kernel's header list, which had fallen
   far behind, is a wildcard.
3. **`fstat` on a FAT file read its size from the vector table**: it
   treated `f->priv`, a handle number plus one, as a pointer. The only
   test asked for a size greater than zero, which any nonzero memory
   passes, and memtest then mapped a file of that "size". The check now
   compares against `stat` and against seeking to the end; with the old
   code it fails, along with ten memtest checks.
4. **`unlink` wrote its deletion through an uninitialised `struct dir`**
   -- into whichever directory the stack named -- and took its name
   with `name_to_83`, so `/X.TXT` could not be unlinked at all.
5. **`unlink` and `rename` looked every name up in the root**, whatever
   the path or working directory; `rename` then wrote into the source
   directory at the slot the root lookup found. Everything was tested
   in the root. `rename` is POSIX's now: it **replaces** an existing
   file (an editor's save depends on that) and really **moves** between
   directories, rewriting a moved directory's `..`. `entry_is_open`
   compared slots without directories.

`fstest.sh` gained six checks for 4 and 5, verified with mtools and
`fsck.fat`; without the `..` rewrite, the `..` check and fsck both fail.

**Tests:** `kernel/libctest.sh`, a new suite: `libc/test/libctest`
makes 64 checks the way a foreign program would -- printf and scanf,
malloc of 8 and 16 MB, qsort, setjmp, the maths library on the FPU,
stdio files, `O_APPEND`, `O_EXCL`, rename and unlink, errno and
strerror, opendir with `.`/`..`, executable bits from `stat`, fork and
exec and waitpid, pipes and poll, signals by picolibc's numbers (SIGUSR1
is 30 to picolibc and 10 to the kernel), `SA_SIGINFO`, alarm and pause,
`WTERMSIG`, time and strftime, `CLOCK_MONOTONIC`, termios and
`tcgetwinsize`; then runs again with stdout redirected to a file that
the host reads -- the check on a buffered stdout being flushed at exit,
and on fork not duplicating unflushed output. 71 checks. Its first run
failed ten, and each was a real fault: no constructors and no atexit
(the linker script lacked picolibc's `__bothinit_array` and
`.fini_array_onexit`), `si_signo`, `isatty` (`TCGETS2`), and unlink by
path. apitest's `sigtest` replaced its two "refused" checks with eleven
for `SA_SIGINFO`, the kernel's trampoline and the `rt_` calls.

### 14. VFAT long file names — done

**Long names in UTF-8**, up to 255 UTF-16 units, in `fs/fat16.c`'s new
"long names" section:

- `dir_find` looks a name up by its long name or its 8.3 name, ignoring
  case (ASCII), assembling long-name runs as it scans and trusting one
  only if it is complete and its checksum matches its 8.3 entry.
  `path_walk` hands back the last component as a name rather than
  eleven 8.3 bytes, and every caller -- open, stat, unlink, rename,
  mkdir, rmdir, chdir -- goes through `dir_find`.
- `dir_create_named` writes 8.3 alone for a name that already is an
  upper-case 8.3 name, as before; otherwise a long-name run and an
  alias: the upper-cased name itself if it is a free 8.3 name, else
  Windows' `NAME~N`. Runs need consecutive free slots, and a
  subdirectory grows to find them.
- Deletion frees the run (`lfn_delete`). Rename re-creates the entries,
  so a name can gain or lose a long form, and a change of case alone is
  a rename rather than a collision with itself.
- Short names honour the NT lower-case bits, and their bytes above 127
  are decoded as code page 437 (Linux's default, and the screen font's).
- Trailing dots and spaces are dropped, as Windows and Linux vfat do.
- `NAME_MAX` 12 → 255, `PATH_MAX` 64 → 256.

**Found on the way:** the line editor and the tty dropped every byte
above 126, so an accented name could not be typed at all. Both pass
0x80-0xFF now. `ls -l` puts the name last, since long names do not fit
a padded column.

**Tests:** fstest gained twelve long-name checks, all read back with the
host's mtools in a UTF-8 locale: a long name made, renamed and found in
another case; case kept; host-made long names in ASCII and UTF-8 found
by the guest; a guest-made UTF-8 name is Unicode to the host; a long
directory with a long-named file, reached by cd; distinct `~N` aliases;
deletion; `ls`. `fsck.fat` runs over the result. **Negative control:**
with `lfn_delete` left out of unlink, fsck reports "Orphaned long file
name part" for each deleted file. libctest gained four through
picolibc's readdir, stat and unlink. The first run failed six, four of
them the test reading the wrong line of output; the real ones were
`readme.txt` getting `README~1` where Windows and Linux use `README`,
and the dropped high bytes.

### 15. `fsck`, and a clean-unmount flag — done

**The flag** is bit 0 of boot-sector byte 0x25 (Linux's, and what
`fsck.fat` reads): set at mount, cleared at unmount. Windows 95's, the
top bit of `FAT[1]`, is read and set but never cleared -- **the first
version cleared it, and mtools then refused the FAT ("Error reading
FAT")**, which failed nine checks in apitest and libctest that read the
guest's files on the host. fscktest now checks mtools can read a volume
left dirty. `halt`,
`shutdown` and `reboot` now unmount, through a new `vfs_shutdown()`
that does not refuse because the console has descriptors open --
`vfs_umount()` would have, every time.

**At boot**, a volume found not cleanly unmounted is checked and
repaired before anything uses it, and the boot banner says so ("fsck :
not cleanly unmounted; checked N files in M directories: ...").

**The check** is `fat_check()` in `fs/fat16.c`, reached from programs
by a private call, `fsctl` (1003), and from the new `/bin/fsck`
(`-y` to repair; exit 0 clean, 1 repaired, 4 not repaired, 8 could not
check). It compares the FAT copies straight off the disk, walks every
chain from the root marking what it reaches, cuts a chain at the first
link that is out of range, free, bad or already claimed, fits sizes to
chains, fixes `.` and `..`, frees orphaned long-name runs and lost
clusters, and copies FAT 1 over FAT 2 -- fsck.fat's repairs, so host and
guest agree on what repaired means. Repair is refused while a file is
open.

**Tests:** `kernel/fscktest.sh`, a new suite, 22 checks.
`kernel/fatdamage.py` damages an image seven ways (lost clusters, a
cross-link, a chain into a free cluster, an oversized file, an orphaned
long name, a wrong `..`, disagreeing FAT copies) plus the dirty flags.
The host's `fsck.fat` is asked first -- it must object, or the test is
testing nothing -- then the guest repairs (at boot in one session, with
`fsck -y` in another), then `fsck.fat` must be satisfied, the flags
must read clean, and files nobody damaged must be byte for byte intact.
A third session kills the emulator and checks the next boot notices.
**Negative control:** with repair switched off inside `fat_check`, four
checks fail, all of them the host's fsck.fat or the exit statuses. One
check that should have failed then did not -- "N problems, .* repairs"
matched "0 repairs" -- and now requires at least one.

### 16. Build and run uEmacs — done

**uEmacs/PK runs**, as `/bin/em` (`ports/uemacs/`) -- Linus Torvalds'
tree at a pinned commit, fetched and patched by `build.sh` and never
copied here, because its licence is MicroEMACS's and not the GPL. 120 KB
of text against picolibc. The patch is three things: termcap from
`<termcap.h>` rather than ncurses, uEmacs's `itoa` renamed away from
picolibc's, and Linux-only termios flags cleared only where they exist.

**What porting it added to the system:**

- **`libc/termcap/`**: termcap with one terminal compiled in, the VT102
  the screen emulates, `li` and `co` asked of the terminal. BSD-licensed
  because it is linked into uEmacs. Built by `libc/build.sh`.
- **`flock`** (Linux 143): advisory locks held by open file descriptions,
  keyed by inode, released at the last close, blocking unless
  `LOCK_NB` (`vfs.c`); and `flock()` in the m68k backend, since picolibc
  declares it and implements it nowhere.
- **A writer while readers have the file open.** uEmacs holds the file
  open for its lock and then saves over it, and the FAT driver refused
  that with EBUSY, because each handle carried its own copy of the size
  and the chain. They are shared now, in `struct fat_node`; a truncate
  resets every other handle's chain hint.
- **A second picolibc patch**: `struct winsize` visible from
  `<sys/ioctl.h>`, and `FIONREAD`, both of which ported programs expect.

**Tests:** `kernel/uemacstest.sh`, a new suite, 9 checks. uEmacs edits a
file the host made -- a long name, "Notes To Edit.txt" -- by keystrokes
alone: M-> and a line typed, M-< and a word inserted mid-line, save,
quit; the host reads the result with mtools. `apps/vcsnap`, run in the
background, copies `/dev/vcsa` while the editor is up, and the host
checks the text and the reverse-video mode line were on the screen. The
terminal's modes must be back afterwards. It passed on its first run.
libctest gained nine checks for the kernel changes: a reader seeing a
writer's truncate and new contents, and flock's refusals, dup sharing,
LOCK_UN and release on close. **Negative control:** with the old EBUSY
rule back and flock always succeeding, exactly those checks fail.

The screen shot is `ports/uemacs/uemacs.png`, from the test's own
session.

### 17. Build and run vi — done

**vi is neatvi** (`ports/vi/`), Ali Gholami Rudi's vi and ex: complete
-- operators, counts, registers, undo, ex with regular expressions,
windows -- in 9,000 lines of POSIX that write their own escape
sequences. ISC-licensed, fetched at a pinned commit like uEmacs. 143 KB
of text.

**Why not BusyBox's vi**, which `emacs.md` named: it lives inside
BusyBox's `libbb`, whose header pulls in the network headers picolibc
lacks. Building it would have been porting half of BusyBox.

**What it needed:**

- **`ftruncate` and `truncate`** (Linux 93 and 92), which the kernel
  did not have. `struct file_ops` gained a `truncate` member; FAT's cuts
  the chain and forgets every handle's chain hint, or extends the file
  with zeroes through the ordinary write path, so the gap reads back as
  zeroes. Wrappers in the m68k backend.
- **A third picolibc patch**: `<poll.h>` used `__size_t` without
  including the header that defines it.
- **A patch to neatvi**: its client for a named Unix-domain socket is
  compiled out where there is no `<sys/socket.h>`.

**neatvi uses one row more than the terminal reports** -- status line
on the last row, message line one below. Measured with `stty rows 20`:
it draws 21. Harmless on the 30-row screen, and neatvi's own layout
rather than anything here, so left alone.

**Tests:** `kernel/vitest.sh`, a new suite, 9 checks: `G` `o`, `1G` `0`
`4l` `i`, `dd` then `u`, `:%s`, `:wq`, then the host reads the file and
vcsnap's copy of the screen; the terminal's modes afterwards. It passed
on its first run. libctest gained a check of `ioctl(TIOCGWINSZ)` itself,
added while working out where neatvi's extra row came from.

### 18. A resolver, then NTP — done

**The resolver** is `resolve_host()` in lib/ulib (`lib/resolv.c`): a
dotted quad; `/etc/hosts`; `localhost`; then an A query over UDP to each
`nameserver` in `/etc/resolv.conf`, or to the server DHCP handed out if
the file names none (the kernel now reports it: `netinfo.dns`, shown by
`ifconfig`). Two tries of two seconds per server, a random ID each
time, and a reply believed only if its source, ID and question are the
ones asked. CNAMEs need nothing special: a recursive server puts the A
records they lead to in the same answer. `ADDRESS#PORT` names a port
other than 53, as dnsmasq and unbound spell it. **No cache**, and no
`gethostbyname`/`getaddrinfo` -- picolibc has no socket layer for them
to sit in; that is a note for later.

**New programs:** `host NAME`, and `ntpdate [-q] [-p PORT] SERVER` --
SNTP (RFC 4330), offset `((T2-T1)+(T3-T4))/2` from four timestamps, the
clock stepped with `settimeofday`, which writes the M48T59, so it
survives a reboot. `ping` and `fetch` take names.

**Found on the way:** the kernel refused every date after January 2038.
`clock_set` rejected a negative `tv_sec`, and a timeval's tv_sec is
signed although this kernel's time is an unsigned count to 2106.
ntpdate against a server set to 2040 said "cannot set the clock". Only
the absolute-time path changed; negative durations are still refused.

**Tests:** `kernel/dnstest.sh`, a new suite, 18 checks, against
`kernel/netservers.py` -- a DNS server and two SNTP servers on
unprivileged host ports, reached at 10.0.2.2 through QEMU's user-mode
network, so nothing needs the internet. An A record, a CNAME chain two
deep, a name in another case, NXDOMAIN and its exit status, `/etc/hosts`
by an alias, localhost, a dotted quad, a server that never answers (and
was asked exactly twice), a **forged reply with the wrong ID sent ahead
of the real one**, ping by name, a dead name server; `ntpdate -q`
reporting without setting, `ntpdate` setting 2031, and a server past
NTP's 2036 wrap giving 2040. **Negative controls:** one try instead of
two fails the "asked twice" check; accepting any ID believes the forged
6.6.6.6. The 2040 check failed first because the test's own timestamp
was mistyped -- 2213937000 for 2214109800 -- and then because of the
2038 bug above.

### 19. TCP: window scaling, timestamps, SACK, keepalives, `TIME_WAIT` — done

**Buffers first**, because the options are worth nothing without them:
64 KB to send and 128 KB to receive (they were 4 KB), page-allocated per
connection when it is made and freed when it ends. The send buffer
became a ring indexed from `snd_una`.

**The options**, each on only if both SYNs offered it:

- window scaling (RFC 7323), shift 2;
- timestamps, with PAWS and an RTT sample from every echoed timestamp;
- SACK (RFC 2018): up to three blocks reported from the reassembly queue
  (now 64 slots, 24 per connection); an eight-entry scoreboard on the
  sending side; holes below the highest SACKed byte resent during
  recovery; SACKed data stepped over after a timeout.

**Keepalives:** `SO_KEEPALIVE` does something now, with Linux's
`TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` and Linux's defaults. A
connection that stops answering is given up with `ETIMEDOUT`. A probe is
an empty segment one byte behind, which RFC 793 says a receiver must
answer with an ACK -- and this stack did not, so without that fix it
could probe but never be probed.

**`TIME_WAIT`** is 60 s (was 10). A retransmitted FIN in it is
re-acknowledged and restarts the timer; an RST no longer ends it early
(RFC 1337).

**Two test knobs in `netctl`:** `NETCTL_TCPLOSS N` drops every Nth
outgoing data segment (1 drops everything, which is how the keepalive
test makes a peer go silent); `NETCTL_TCPOPTS` stops new connections
offering chosen options. `NETCTL_CONN` reports the agreed options, both
shifts, the largest window seen, retransmission counts and probes sent.

**Tests:** `kernel/tcptest.sh`, a new suite, 24 checks, all over
loopback (`apps/tcptest`):
- 256 KB with every option agreed, a peer window above 65535, an RTT
  measured, and nothing retransmitted;
- a receiver that stalls for 1.5 s still takes more than 64 KB;
- 512 KB at one loss in 25, with SACK and without, both intact, and
  SACK retransmitting less on the same losses;
- every option off, with the window staying inside 16 bits;
- keepalive probing a live peer that stays up, and a silent one given
  up with `ETIMEDOUT`;
- `TIME_WAIT` still present after 15 s, with its port refused to `bind`.

**Loopback cannot catch a mistake made the same way at both ends**, and
this stack is both ends. So `nettest.sh` now captures the guest's frames
with `-object filter-dump` and decodes its SYN options on the host
(`kernel/synopts.py`, straight from the RFCs): MSS 1400, window scale 2,
SACK permitted, a nonzero timestamp. 2 checks.

**Negative controls**, each caught:

| Change | Checks that failed |
|---|---|
| window scaling never agreed | 3 |
| `sacked_until` disabled | "SACK resent less" |
| keepalive probe not sent | "stays up" |
| `TIME_WAIT` back to 10 s | both `TIME_WAIT` checks |
| the RFC 793 ACK to an old segment removed | "stays up" |
| keepalive never gives up | "given up: ETIMEDOUT" (a hang, until that read got a timeout) |
| window scale sent as option 30 on both sides | **none of tcptest's 24**; nettest's SYN decode |

The last row is why the SYN decode exists.

**Found on the way -- a harness bug in five suites, older than this task:**
`apitest`, `fstest`, `nettest`, `vmtest` and `edittest` did `cd
"$(dirname "$0")"` and then sourced `"$(dirname "$0")/../machine.conf"`,
a path relative to the directory just left. Run as `make test` runs
them (`cd kernel && ./x.sh`), that is `./../machine.conf` and works; run
by path from the root it is not found, `RAM_MB` is unset, and `set -u`
kills the backgrounded QEMU line during expansion -- **before its
redirection truncates the log**. The checks then graded the previous
run's log and passed. All five now source `../machine.conf`, `runtest.sh`
likewise, and every suite deletes its log before starting QEMU, so a run
that never reaches the guest has nothing to grade.

### 20. Shared libraries — done

**The ELF model, unchanged:** a dynamic program names `/lib/ld.so` in
`PT_INTERP`; the kernel loads both, puts an auxiliary vector after the
environment, and starts `ld.so`, which loads the `DT_NEEDED` libraries
(`LD_LIBRARY_PATH`, then `/lib`) with `mmap`, relocates everything --
`R_68K_32`, `PC32`, `GLOB_DAT`, `JMP_SLOT`, `RELATIVE` and `COPY` --
runs the libraries' initialisers and jumps to the program.

- **`ld.so`** (`ldso/`, 4.6 KB, freestanding) is linked `ET_EXEC` at
  0x1fe00000, which Linux also accepts, so nobody relocates it. Binding
  is eager: a missing library or symbol is refused before `main`, status
  127. `LD_TRACE_LOADED_OBJECTS` works as `ldd` does. Function addresses
  are canonical across modules, by the ELF rule.
- **`libc.so`** is picolibc built a second time with `-fPIC`
  (`libc/build.sh`, `libc/libc-so.ld`). libgcc is not PIC, so its code
  is renamed and put in the data segment; the text has no relocations,
  and `build.sh` refuses the library if it has any, or leaves a symbol
  undefined.
- **`crt0-dyn.s`** runs the program's own constructors (libc.so's
  `__libc_init_array` walks libc's) and registers its destructors with
  `atexit`, so they run before libc's flush of stdout.
- **`LINK=dynamic`** in `libc.mk`, and `NAME.dyn` targets. **uEmacs and
  vi are now dynamic**: 113 KB and 150 KB on disk, from 169 and 203.

**Sharing the pages** -- the point, since disk is not scarce:
- `pmm.c` keeps a reference count per page (holders beyond the first,
  so every existing `pmm_free` caller is unchanged);
- `textcache.c` holds one copy of each page of a file mapped privately
  and read-only, keyed by inode and offset, and `mmap` takes those pages
  from it. The VFS makes it forget a file on write, `O_TRUNC`, truncate,
  unlink, rename, fsck repair and unmount;
- `fork` shares every page neither side can write;
- `vm_protect()` copies a page anyone else holds before making it
  writable -- copy-on-write, at the one moment write can be granted.

`memctl` (1004) reports the cache and, for tests, the physical page
behind one of the caller's own addresses.

**Tests:** `kernel/sotest.sh`, a new suite, 117 checks:
- `sotest` (`libc/test/sotest.c`) against `libsot.so` and `libc.so`: a
  library's function, variable (COPY), constructor, and its own call
  into libc; one address for a function everywhere; libc's text on the
  SAME physical page in a separately exec'd program and in a forked
  child, found in the cache; the library's data private; `mprotect`
  giving one process its own copy while the child keeps the shared one.
- from the shell: a library replaced by `cp`, and rewritten in place
  without truncating, seen by the next program; a mapping of a file
  truncated and not written showing zeroes; a file moved into the slot
  (and so the inode number) of a deleted one, and one moved over an
  existing file, each showing its own bytes -- with a check that the
  inode number really was reused, without which those two would prove
  nothing; `LD_LIBRARY_PATH`; `LD_TRACE_LOADED_OBJECTS`; a missing
  symbol, a missing library, a missing `ld.so` (`ELIBACC`).
- libctest, all 90 checks, linked dynamically.

**Negative controls**, all caught: mmap not using the cache (4 checks);
no forget on write, on `O_TRUNC`, on unlink, on rename (one each -- the
last two only after the harness was made to reuse an inode, since a
rename within a directory keeps its slot); no copy in `vm_protect`
(1); `fork` copying everything (2); `ld.so` without canonical function
addresses (1).

**Found on the way:**
- **This gcc passes neither `-static` nor `-shared` to the linker.**
  With libc.so installed beside libc.a, `libc.mk`'s "static" link was
  producing a dynamic program asking for `/usr/lib/libc.so.1`, and
  `-shared` made an executable. Both now go as `-Wl,`, and `libc.mk`
  refuses a static program with a `PT_INTERP`. The ports' build scripts
  had the same bare `-lc` and would have broken on their next build.
- **picolibc 1.8.12's `cfsetspeed.c` defines `cfsetospeed`** -- so there
  is no `cfsetspeed`, and linking the whole library fails -- and none of
  the three `cfset*speed` returns a value (`patches/cfsetspeed.patch`).
- **`clock_getres` existed nowhere**, kernel or libc, though picolibc's
  `timespec_getres` calls it.
- **libctest exec'd `/LIBCTEST` by name**, so its dynamic build tested
  the static one's exit status; it now execs itself.
- **`struct sysinfo` was not Linux's.** It was a cut-down private
  layout under Linux's number, so a Linux program read its fields from
  the wrong offsets. It is Linux/m68k's now, 64 bytes, with `bufferram`
  counting the cache's idle pages and `free` showing them as "cache" --
  which is what apitest's leak check needed once the cache kept pages
  after a program exited: used before 192, after 192.

### 21. Paging and swapping — done

**Demand paging.** An access fault from a program goes to `vm_fault()`
first; because the 68040 pushes the address of the faulting
instruction, returning re-runs it, and that is all demand paging needs
from the processor. Three kinds of absent page, each an invalid
descriptor the fault path understands:
- **lazy** (`SW_LAZY`) -- stacks, the heap, anonymous `mmap`: a zeroed
  page on first touch. A program's 1 MB stack cost 256 pages at exec;
  a running `spin` now holds 7. `mmap` of 32 MB costs its 18 pages of
  tables.
- **swapped** (`SW_SWAP`, slot in bits 31..12);
- **copy-on-write** (`SW_COW` on a resident, write-protected page):
  `fork` shares every page, and a 1 MB process forks for 6 pages.

`uaccess` calls `vm_fault()` too: the kernel walks tables rather than
touching, so it must do what a fault would.

**Swap** is a file (`swapon`/`swapoff`, Linux's 87 and 115, and
`/bin/swapon`, `/bin/swapoff`): its clusters are turned into sectors
once, by a new filesystem `bmap`, and the kernel then reads and writes
them directly. While on, the file refuses writes, truncation, renames
and deletion (`ETXTBSY`). Replacement is the clock algorithm over the
MMU's used bits, across every address space, run only where a page is
about to go to a program. Only pages with one holder are taken -- which
is also how **pinning** works: `read`/`write`/`send`/`recv` hold a
reference to each user page for the length of the call, since a read
from a pipe can sleep holding its physical address. Allocation is first
fit. `mmap` and `brk` refuse what memory and swap together could not
supply; past that, a page that cannot be had kills whoever faulted
(SIGKILL, 137).

`sysinfo` reports `totalswap`/`freeswap`, `free` a swap line, and
`memctl` the fault and swap counters.

**Tests:** `kernel/pagetest.sh`, a new suite, 48 checks, two machines:
64 MB without swap (laziness, copy-on-write both ways, the commit
refusal, two overcommitted processes killed and the machine not, every
page back) and 12 MB with 24 MB of swap (16 MB filled and read back by
one process and by two; a fork with pages out, the parent rewriting
everything while the child must still see the old; a read blocked on a
pipe while another process thrashes; the swap file refusing rm, cp and
mv; swapoff refused with too much out -- the parked process's pages
intact -- then allowed once it has gone). Every graded program must
reach its own "N failed" line: before that rule a pagetest killed by
the OOM path passed, because it printed no FAIL lines.

**Negative controls**, all caught (checks failed):

| Change | |
|---|---|
| stack mapped whole at exec | vmtest 1 |
| anonymous mmap eager | 7 |
| fork copies instead of sharing | 2 |
| copy-on-write fault without the copy | 7 |
| eviction that does not write the page | 8 |
| swap-in that does not free its slot | 5 |
| no pinning in read/write | 1 -- only once the filler thrashed for the whole wait |
| fork not taking a hold on a swapped page's slot | 1 -- only once allocation was first fit and the parent rewrote everything |
| exit keeping the address space until reaped | 1 |
| no commit check | 2 |

**Found on the way:**
- **A zombie held all its memory until reaped** -- every page, then
  every swap slot. The address space goes at exit now, as Linux's does.
- **The fault loop guard killed a fourth run of the same program**, by
  counting identical first-touch faults in a reused task slot. It counts
  only `VM_FAULT_NOCHANGE` now.
- **`memctl`'s stats struct grew and overran sotest's shorter copy** --
  a crash at pc=2. It takes the caller's size now.
- **edittest's ctrl-C check was racy** (about 1 in 10 under load): its
  0x03 could arrive while the shell was still running the previous
  command, and a ctrl-C nobody is reading for is discarded. A pause
  before it.
- vmtest and apitest checked that memory was taken at `mmap`/`brk`/exec
  time; they check it is taken when TOUCHED now.

### 22. Interrupts, the NVRAM, the limits — done

**Input.** The serial port and the keyboard take their interrupts
through the MFP (GPIP5 and GPIP1, rising edge). The handler drains the
chip into a 256-byte ring in `tty.c`, acting on ctrl-C and ctrl-Z at
once. A full ring STOPS draining and leaves the rest in the chip --
whose full FIFO makes the sender wait -- rather than dropping: the first
version dropped, and fstest's typed-ahead commands lost their middles
(`hello one two` arrived as `twraB.The.`). Sources with no interrupt,
the console's own replies, are still polled.

**The disk.** GPIP4. Up to 256 sectors a command; between sectors the
task waiting SLEEPS, and the machine runs something else. At boot, in
the idle task, it polls (`task_can_sleep()`). A 5 s limit on every wait.
A mutex keeps two tasks' requests apart.

**The filesystem lock.** A task can now be asleep in the MIDDLE of a
FAT operation, which was impossible before -- so every call from the VFS
into the filesystem holds a recursive lock, taken only for the
filesystem's own files and calls, never a pipe's or a terminal's. Lock
order: filesystem, then disk; swap I/O takes the disk alone.

**The NVRAM**: `/dev/nvram` (8176 bytes, read/write/lseek), `/bin/nvram`
(`KEY=VALUE` settings), `ifconfig nvram`. Under QEMU it survives a
machine reset, not the emulator exiting.

**Limits**: 64 tasks, 64 descriptors each, 256 open files and 128 open
FAT files machine-wide (it was 32 -- the whole machine), 64 sockets, 32
TCP connections, 128 pipes, 64 in `poll`. `fork` says `EAGAIN` when the
task table is full, as Linux does; it said `ENOMEM`.

**Also:** `kstat` (1005), and `/bin/irqs`: interrupts per MFP channel,
spurious ones, and whether the disk's waits slept or polled.

**Tests:** `kernel/devtest.sh`, a new suite, 23 checks: serial, keyboard
(typed through the monitor's `sendkey`) and disk interrupts counted; a
disk wait sleeping; six processes in one directory with every disk
request slowed to 15 ms by a test knob, every byte checked and the
volume checked with the host's `fsck.fat`; every limit filled and
passed (`apps/limits`); NVRAM settings written, deleted, surviving a
reset -- this QEMU runs without `-no-reboot` -- and configuring the
interface.

**Negative controls**, all caught:

| Change | |
|---|---|
| no serial interrupt | the serial count stays 0 (input still works, polled: by design) |
| no keyboard interrupt | 3 |
| the disk never sleeps | 1 |
| no filesystem lock | 2 -- files corrupted and fsck.fat errors -- but ONLY with the delay knob: at QEMU's speed the windows are too short, and without the knob this control passed |
| the input ring drops when full | fstest, 34 |
| fork's ENOMEM for a full table | 2 |
| FAT open files back to 32 | 3 |
| NVRAM writes lost | 4 |
| (task 21's) eviction writing before unmapping, as it did | pagetest 4 -- a fork child saw its parent's later writes |
| (task 21's) no copy-on-write eviction | pagetest 1 |

**Found on the way** -- most of it by the rest of the suite, because
the disk sleeping changes what can happen while anything waits for it:
- `mfp_init()` runs after the serial port and the disk are brought up
  and clears every MFP handler and enable -- the first version's serial
  port went deaf with no error anywhere; each now has an `*_irq_on()`
  called after it.
- **uaccess's address-space override was a global.** exec sets it while
  filling a new program's memory, and exec now sleeps on the disk; a
  pipeline's first stage ran meanwhile and read and wrote the SECOND
  stage's half-built memory. Per task now (`ua_override`).
- **ctrl-C at the prompt was lost.** The interrupt handler acted on it by
  signalling the foreground group -- the shell's, which has no program
  in it, so nobody -- where the polled path had let the line editor see
  the character. It now acts only when there is a program to act on,
  and otherwise queues the character for the reader.
- **ctrl-C while a pipeline was being started reached nobody.** Starting
  a stage sleeps now, so the stages already started run and print
  before the shell hands the terminal over; a ctrl-C then went to the
  shell's group and the shell waited for ever. The terminal now keeps
  it (`pending_sig`) and delivers it to the group it is handed to next,
  once every stage exists.
- **Eviction wrote a page out while it was still mapped** -- harmless
  while swap I/O could not sleep, a lost write or a double eviction once
  it could. The page leaves the map first, its slot is marked busy for
  the write, and every path that sleeps re-checks the descriptor after.
  pagetest runs `pair` a second time with every disk request slowed
  (the delay knob) to keep this honest.
- **After a fork nothing could be evicted**: every resident page had two
  holders, and reclaim took only pages with one. A copy-on-write page is
  now taken from one space at a time. Found by `forkswap` failing on its
  second run in one boot; `forkswap` now reads everything back before
  forking, so that memory is full of shared pages when it does.

### Notes for later: sockets and names for picolibc — done

`libc/net/`, BSD-licensed like the rest of `libc/`, built into
`libc.a` and `libc.so`: `<sys/socket.h>`, `<netinet/in.h>`,
`<netinet/tcp.h>`, `<arpa/inet.h>`, `<netdb.h>`, `<sys/un.h>`,
`<sys/uio.h>`; every socket call; `inet_*`; and a resolver --
`getaddrinfo`, `getnameinfo`, `gethostbyname`, `getservbyname` -- over
`/etc/hosts`, `localhost` and DNS, with a per-process cache that keeps
answers for their TTL and "no such name" for 30 seconds.

**Found:** picolibc's `struct timeval` has a 64-bit `tv_sec`, the
kernel's 32. `SO_RCVTIMEO`/`SO_SNDTIMEO` would have been read as the
high half -- zero -- so every socket timeout would have been none; the
wrappers convert.

**Tests:** `libc/test/inettest.c`, 39 checks, run by `dnstest.sh`
against its DNS server: the text forms, every lookup path including the
cache (counted: the server sees `foo.sage.test` exactly twice across
`host` and inettest), a forged reply ignored, the error codes, TCP and
UDP over loopback through the headers a port uses, a 1.5 s receive
timeout that times out.

**Negative controls**, all caught: the timeval passed through
unconverted (2); no cache (4); no ID check on replies (2).

### Notes for later: the stack, the framebuffer, the shell — done

**Kernel stack high-water mark.** Every kernel stack is painted with a
pattern when a task is made, and `task_reap()` measures how much of it
was overwritten. `KSTAT_STACK` reports the deepest ever and which task
reached it, and `irqs` prints it. After devtest's whole session --
the stress run, the limits, a reset -- it is **4,104 of 8,192 bytes, by
sh**; devtest fails if it passes three quarters.

**`mmap` of `/dev/fb0`.** `struct file_ops` has an `mmap` member, last,
through which a device names the physical page behind an offset;
`fb.c` answers from the SM501's video memory. `do_mmap` maps those
pages as themselves, uncached and shared, allocating nothing.
Nothing had to mark them as not-owned: `pmm_free` already ignores
an address outside its pool, reclaim already skips a page with no
reference count, and `vm_clone` now hands the child the same device
page instead of copying it. `FBIO_GETINFO` gained `mem_size`,
`draw_offset` and `show_offset`.

**Tests** (devtest, `apps/fbmap.c`): the mapping costs 9 pages of page
tables for 16 MB and none of RAM; the page behind it is 0xf0000000;
what the program and its forked child wrote reads back; munmap gives
back nothing. Then a screendump through the monitor, read on the host:
red where the parent drew, green where the child did, black between.

**Found:** the first screendump was black. Not a mapping fault -- the
console was on the screen, and fbmap's own ten lines of results
scrolled the boxes off the top. devtest takes the console off the
screen (`console fbcon off`) for that one program.

**Negative controls**, all caught by devtest:

| control | failures |
|---|---|
| `vm_clone` copies device pages, as it would RAM | 3 -- the child's green never reached the screen |
| `fb_mmap` refuses | 4 |
| a page of RAM mapped where the device's should be | 6 -- the cost, the physical address, and the screen |

**The shell's command lists.** `;`, `&&`, `||`, and `&` between
commands, outside quotes, with `2>&1` still a redirection. Everywhere
the shell reads a command: the prompt, scripts, `if`, `sh -c`.
**Found:** builtins never set `$?`, so `cd /nowhere && echo yes`
printed yes. A builtin that reports an error now returns 1, `test`
returns its answer, and everything else 0. Four harnesses had been
checking `$?` after a builtin and so were passing on the stale value;
they now test `cmd; echo X=$?` on one line. edittest has seven checks
of the lists; the controls (no list parsing; builtins not setting
`$?`) fail them.

### A loopback interface — done

127.0.0.1 has worked since task 10, but as a special case in
`ip_output` with no interface behind it: nothing to list, nothing to
count, nothing to take down. It is **`lo`** now -- 127.0.0.1/8, its own
RX/TX counters, up or down -- beside `eth0`. `NETCTL_INFO`, `NETCTL_UP`
and `NETCTL_DOWN` take an interface number (0 eth0, 1 lo; UP and DOWN
had been defined and never implemented), `struct netinfo` gained
`loopback`, and `ifconfig` shows every interface, or `ifconfig NAME`
one, and `ifconfig NAME up|down`. lo exists with no card at all, and
ping no longer refuses to run without one. A down lo makes 127/8
`ENETUNREACH`, and ping now says why a packet was never sent rather
than calling it "no reply".

**Found: 127/8 was accepted from the wire.** `ip_input` took any
datagram addressed to 127/8 whatever it arrived on, so anything on the
LAN could reach a service bound to 127.0.0.1 by sending a frame to
this machine's MAC. Martians -- 127/8 as destination OR source, off
the wire -- are dropped and counted now; `ip_input` is told whether a
frame came off lo.

**Tests:** `kernel/lotest.sh`, 22 checks, on a wire the test owns:
QEMU's `-nic socket,udp=...` carries each frame as a UDP datagram, so
`kernel/wire.py` both captures everything the guest sends and puts
hand-made frames on the wire. lo listed, addressed and counted exactly
(6 each way for three pings; 4 for two pings of the machine's own
address); eth0 sent nothing for any of it and no frame with a 127/8
address appeared on the wire -- next to a ping of a neighbour whose ARP
DID appear, so the capture is known to be looking; lo down and up;
and each martian beside an ordinary datagram sent the same way that
arrives (`apps/udpwait`).

**Negative controls**, all caught:

| control | failures |
|---|---|
| no martian filter | 3 |
| lo's sends not counted | 1 |
| lo down ignored | 2 |
| the machine's own address sent to the wire | 3 |
| lo's frames counted as eth0's | 2 |

### Regression tests: two flakes in pagetest's out-of-memory check

Found by a full-suite run, and both in the test, not the kernel:
- **"every page came back" measured too soon** when the kernel chose
  to kill the PARENT: its child, orphaned half way through touching
  48 MB, was still running when `free` ran (12,344 pages used, 4 jobs).
  The harness now polls `free` until the job count is back to where it
  started. Confirmed by forcing the case: every parent-killed run gave
  back every page within two polls.
- **One run in six nobody ran out of memory at all** (`OOM=5`): the
  child touched all of its memory and exited before the parent began,
  so the two never overlapped. The processes now touch half, meet
  through a pair of pipes, and touch the rest -- past the meeting both
  cannot finish -- and 8 runs in 8 killed one of them.

### 24-29. The standard tools: awk, sed, grep, bash, the utilities — planned

Measured before starting (2026-09-22), against the built libc
(`libc.a` + `liblinux.a`) and the kernel's dispatch tables:

- **Present already**: `fork`/`vfork`/`execve`, `waitpid`/`wait4`,
  `pipe2`, `dup3`, `fcntl`, `sigaction`/`sigprocmask`/`sigsuspend`,
  `setpgid`/`getpgrp`, `TIOCGPGRP`/`TIOCSPGRP`, termios, `regcomp`,
  `fnmatch`, `getopt_long`, `setlocale`/`mbrtowc`/`wcwidth`, `mmap`,
  copy-on-write fork. 256 arguments and 256 environment strings.
- **Missing from the libc** (task 24): `glob`, `sigsetjmp`/`siglongjmp`,
  `realpath`, `uname`, `getrusage`, `fchmod`, `fchdir`, `link`,
  `utime`/`utimes`, `mkfifo`/`mknod`, the `chown` family, `openat`/
  `fstatat`/`unlinkat`/`faccessat`, `posix_spawn`, `getdtablesize`.
  Most are wrappers: the kernel has `uname`, `wait4`, `openat`,
  `unlinkat`, `faccessat`, `getrlimit`.
- **Missing from the kernel** (task 24): `getrusage`, `fchdir`,
  `fchmod`, `utimensat`, `mknod`, `chown`, `link`, `sigaltstack`. On
  FAT `link`, `chown` and `mknod` can only fail honestly or do nothing.

**Plan.** awk first -- Kernighan's, not gawk: plain C, no configure,
and it proves the build path. Then GNU sed and grep through a cross
`./configure --host=m68k-elf` with a cache of answers; gnulib fills
most gaps itself. bash last: `sigsetjmp` everywhere, `wait3`/`wait4`,
`getrlimit` for `ulimit`, `getrusage` for `times`; process substitution
configured out at first (no `/dev/fd`, no FIFOs). Linked dynamically
against `/lib/libc.so`. bash installs as `/bin/bash` beside the
existing shell, not in place of it. Each is fetched at a pinned
version and patched by a build script, as uEmacs is -- sources are not
copied into the tree. bash's own test suite doubles as a kernel
regression test.

**Task 29, added 2026-09-22: the small utilities** -- sort, wc, find,
xargs, head, tail, cut, tr, uniq, tee, cmp, comm, od and the rest.
From **sbase** (suckless.org): about a hundred POSIX tools, each small,
plain C99 and POSIX, MIT-licensed, built by one Makefile -- rather than
GNU coreutils (gnulib and a cross configure for every one) or BusyBox
(one GPL-2 multicall binary with its own configuration). Where one
duplicates a shell builtin (`ls`, `cp`, `rm`, `echo`), the builtin still
runs from the prompt; the program is what scripts, `xargs`, `find -exec`
and awk's `system()` reach. Needed already: awk's own `space` test pipes
into `sort`.

### 30. POSIX gaps found along the way — to do

Found while porting awk, sed, grep and sbase. Those marked (29) are being
done as part of task 29, because sbase's utilities need them. The rest
are to be done too, whether or not anything needs them yet: task 31
(Python) will need most of them, and anything done now makes it easier.

**Kernel**
- (29) Setting file times: `utimensat`/`futimens`. FAT keeps a
  modification time; `touch` and any `make` need to set it.
- (29) Sessions: `setsid`, `getsid`. Process groups exist; sessions and
  a controlling terminal do not.
- (29) A settable host name: `sethostname`, and `uname`'s node name from
  it (now fixed at "sage040" in the C library).
- Scheduling priorities: `getpriority`/`setpriority`, so `nice` and
  `renice` mean something. The scheduler is plain round robin.
- `sigaltstack` (`SA_ONSTACK` is refused).
- `/dev/random` and `/dev/urandom` need a cryptographic generator;
  `random.c` is xorshift, which is why neither device exists.
- `PATH_MAX` is 256 in the kernel and 1024 in picolibc's headers: a
  program sizing buffers from PATH_MAX is fine, one trusting it gets
  ENAMETOOLONG past 256. Raise the kernel's, or make them agree.
- `execve` takes at most 256 arguments, a limit POSIX cannot express
  (ARG_MAX is bytes); `xargs` builds command lines by bytes.
- FIFOs, device nodes and links cannot live on FAT: `mkfifo`, `mknod`,
  `link` and `symlink` fail with EPERM. Named pipes could be kept in
  memory by the VFS instead.
- `chroot`: there is one mounted filesystem and no reason yet.
- `/dev/fd` (or FIFOs) for bash's process substitution `<(...)`.

**C library (picolibc)**
- (29) `fdopendir`, `fchownat`, `fchmodat`, `linkat`, `symlinkat`,
  `sync`, `confstr`, `clock_settime`, `mkdtemp`, `utime`/`utimes`.
- (29) Headers: `NZERO` in `<limits.h>`, `UTIME_NOW`/`UTIME_OMIT` in
  `<sys/stat.h>`, `<sys/sysmacros.h>` (`major`/`minor`/`makedev`).
- `glob()`, `posix_spawn()`.
- `struct tm` has no `tm_gmtoff` or `tm_zone`; sbase's `touch` uses
  them for a trailing `Z` (UTC).
- `SI_USER` is 1 in picolibc and 0 on Linux, and si_code is passed
  through from the kernel, so a handler testing for SI_USER misjudges
  a kill().
- `sysconf` has no `_SC_NPROCESSORS_*` or `_SC_PHYS_PAGES`: picolibc's
  `<unistd.h>` does not define the names.
- `cfsetspeed` is defined but not declared in `<termios.h>`.
- No `<syslog.h>` and no syslog daemon: sbase's `logger` and `cron`
  are not built.
- Time zones: not checked whether `localtime` honours `TZ`.

**Tools and tests**
- grep has no `-P` (no PCRE).
- sed's and grep's own test suites are shell scripts over a POSIX shell
  and coreutils: run them once bash and sbase are in (28, 29).
- awk's `space` and `system-status` tests are skipped until `sort`, a
  POSIX `kill -SIGNAME`, `$$` and `VAR=value cmd` exist (28, 29).
- sbase's `nice`, `renice`, `logger`, `cron` and `chroot` are not built
  (above).

### 31. Python — to do

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

## Decisions worth knowing about

- **RAM 64 MB, disk 512 MB**, single-sourced in `machine.conf`. The
  emulator allows 2 GB and the kernel scales to it; neither is a ceiling.
- **The kernel's boot stack is in the image**, a reserved section below
  `_end`, which lets the allocator own everything above.
- **The pmm bitmap is placed, not allocated**, at the front of its pool.
- **VT102, not VT100**, for the console — see above.
- **Nothing is on a "deliberately not doing" list any more.**

## Notes for later

- **Kernel stack use -- measured.** 4,104 of 8,192 bytes at most, by
  the shell, after devtest's whole session. Logged above.
- **`mmap` of `/dev/fb0` -- done.** Logged above.
- **The shell's `;`, `&&` and `||` -- done.** Logged above.
- **A loopback interface, `lo` -- done.** Logged above.
- **Names for picolibc programs -- done.** `libc/net/`: the socket
  headers and calls, `inet_*`, and a resolver with `getaddrinfo` and a
  per-process cache that honours TTLs (see `libc/README.md`). lib/ulib's
  resolver deliberately has NO cache: every ulib program that resolves
  a name -- `host`, `ping`, `ntpdate`, `fetch` -- does it once and
  exits, so a per-process cache could never be hit. A cache the whole
  machine shares would need a daemon to hold it; that is a separate
  thing, not a note.
