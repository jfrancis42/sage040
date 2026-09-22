# Progress

Working log for the port-enablement work: making Sage040 able to build
and run software written by other people, with a real editor as the
proof. `design.md` §11 is the reasoned list; this file is the order, and
the running state as it actually is.

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk — both can be increased
and have been.

**Status: 20 of 23 complete.**

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
| 20 | Shared libraries | downstream of `mmap` and the libc | todo |
| 21 | Paging and swapping | downstream of `mmap` | todo |
| 22 | Interrupt-driven input **and disk**, the NVRAM, the static limits | cleanup, any time | todo |
| 23 | Regression tests throughout | every task ships with its tests | ongoing |

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

## Decisions worth knowing about

- **RAM 64 MB, disk 512 MB**, single-sourced in `machine.conf`. The
  emulator allows 2 GB and the kernel scales to it; neither is a ceiling.
- **The kernel's boot stack is in the image**, a reserved section below
  `_end`, which lets the allocator own everything above.
- **The pmm bitmap is placed, not allocated**, at the front of its pool.
- **VT102, not VT100**, for the console — see above.
- **Nothing is on a "deliberately not doing" list any more.**

## Notes for later

- **The disk should be interrupt-driven** (task 22). `drivers/ata.c` is
  polled PIO although the IRQ is wired to MFP channel 6 (GPIP4, per
  `hw/m68k/sage040.c`); its "no scheduler to block" reason is gone. With
  a non-preemptible kernel a polled wait stops *every* task for the whole
  request — short under QEMU, milliseconds on a real drive. **Known
  traps:** `mmio-ide` takes its IRQ by value at realize, so connect it
  before `sysbus_realize_and_unref()`; the sleep needs a timeout so a
  dead drive is an error, not a hang. `t3-ata` should gain an interrupt
  check.
- **Kernel stack use is unmeasured.** `do_syscall`'s frame was 1,840
  bytes with `do_spawn` inlined; task 6 cut it to 624, against an 8 KB
  stack. A high-water mark (paint the stack, check at exit) would say
  how much margin there is.
- **`mmap` of `/dev/fb0`.** `mmap` exists; missing are a `file_ops` hook
  through which a device offers physical pages, and a descriptor mark
  for pages *not* owned so `vm_destroy` never hands VRAM to the
  allocator. Three documents still give missing `mmap` as the reason. Not
  on the editor's path.
- **The shell has no `;`, `&&` or `||`.** Pipelines and redirection
  work; command lists do not, so `a; b` passes `; b` to `a`.
- **Names for picolibc programs**: `getaddrinfo`/`gethostbyname`, which
  need a socket layer in picolibc first (`<sys/socket.h>`,
  `<netinet/in.h>`, wrappers onto the calls the kernel already has).
  The resolver is lib/ulib's only. It also has no cache.
