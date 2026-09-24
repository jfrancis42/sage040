# SuckOS

`README.md` describes the machine — a 68040, its chips, and the QEMU model
that provides them. This document describes the system that runs on it:
what it is, how it is put together, and why each piece is the way it is.

SuckOS has protected address spaces, preemptive multitasking, demand paging
and swap, signals with job control and sessions, a filesystem, a TCP/IP
stack, a cryptographic random generator and a shell, and a program it runs
cannot bring it down. Its system call interface is Linux/m68k's, which is
what lets ordinary POSIX programs build against it: GNU bash, GNU sed and
grep, the one true awk, uEmacs, vi, and 98 of suckless's utilities all run
on it, built from their own unmodified sources against picolibc -- and so
does **CPython 3.14**, with its standard library on the disk.

It is not a Unix clone and it is not a Linux. Its filesystem records who
owns a file and what may be done with it, and nothing checks either yet.
**What it is not**, at the end of this document, says where the edges are;
`progress.md` is what is still to be built.

---

## Contents

- [Shape of the thing](#shape-of-the-thing)
- [Booting](#booting)
- [Memory](#memory)
- [Tasks](#tasks)
- [Waiting](#waiting)
- [Signals and job control](#signals-and-job-control)
- [System calls](#system-calls)
- [Time zones](#time-zones)
- [Files](#files)
- [Randomness](#randomness)
- [The terminal](#the-terminal)
- [Pseudo-terminals](#pseudo-terminals)
- [Users](#users)
- [The log](#the-log)
- [Doing something later](#doing-something-later)
- [The network](#the-network)
- [Programs](#programs)
- [The shell](#the-shell)
- [Testing](#testing)
- [What it is not](#what-it-is-not)

---

## Shape of the thing

About 21,000 lines of C and a little assembly, in `kernel/`:

```
kernel/
  main.c          boot, device discovery, the init task
  task.c .h       the scheduler
  taskasm.s       the context switch, and nothing else
  wait.c .h       wait queues, semaphores, mutexes
  signal.c .h     signals and their default actions
  trap.c          exception entry, faults, the system call gate
  syscall.c .h    the system call table
  uapi.h          the ABI: numbers, structures, constants
  exec.c          loading a program
  execasm.s       entering one
  pmm.c           the physical page allocator
  vm.c .h         address spaces and the MMU
  uaccess.c       reaching into a program's memory, safely
  vfs.c           mounts, paths, descriptors
  tty.c           the terminal: line discipline, sources, sinks
  console.c       kernel printing
  fb.c fbcon.c    the framebuffer, and a text console on it
  dev.c .h        the device model
  time.c timer.c  the clock and the tick
  random.c        numbers a remote party cannot guess
  shell.c         the shell
  edit.c          its line editor
  drivers/        ns16550 i8042 ata m48t59 mfp sm501 smc91c111
  fs/fat16.c      the filesystem
  net/            arp ip icmp udp dhcp tcp socket
```

Three rules hold the shape, and two of them are enforced by the build
rather than by good intentions:

**The shell is not the kernel.** `shell.c` and `edit.c` may include
`syscall.h` and a short list of pure headers, and nothing else.
`kernel/layercheck.sh` runs before every link and fails the build
otherwise. The rule stood in three documents for months while
`cmd_console` quietly called `tty_sink()` directly, which is why it is now
a script. When the shell genuinely needs something the answer is a system
call or an ioctl.

**Only `main.c` names a chip.** Everything else talks to `dev.h`. A driver
registers; a subsystem asks for "a block device" or "the clock".

**The system call interface is Linux's**, deliberately: `d0` holds the
number, `d1`–`d5` the arguments, and `d0` comes back holding the result or
a negated errno. The numbers are the real Linux/m68k ones where a call
exists there. Nothing is renumbered for tidiness, because the value of
matching is that it stops being a decision.

---

## Booting

The boot ROM (`bootrom/`) is not part of the OS. It sizes memory, finds the
IDE disk, reads the partition table, loads `/KERNEL.ROM` from the ext2
filesystem and jumps to it. The kernel can also be loaded directly with
QEMU's `-kernel`, which is faster to iterate on and skips the ROM
entirely; `make run` does that and `make boot` goes through the ROM.

Then, in `main()`, in this order and for reasons:

```
pmm_init()        the page allocator, over whatever RAM was found
vm_init()         page tables, the kernel's identity map, MMU on
task_init()       the task table, and task 0
trap_init()       the vector table and the exception handlers
check_syscall_gate()
probe_all()       bus-error probes for every device
start_memory()
start_drivers()   uart, keyboard, disk, clock, framebuffer
report_console()
start_network()
mount_root()
```

**`task_init()` must come before any driver.** Descriptors live in a task,
and `tty_init()` binds 0, 1 and 2. With no current task that wrote through
a null pointer into the vector table — which is mapped and writable, so it
did not fault. The machine booted, printed its banner and died at the first
exception. That is the kind of ordering dependency this list encodes.

**Every driver probes before it touches a register.** A device that is not
there raises a bus error; it does not read back zeroes. `memprobe.s`
provides `io_probe8/16/32`, and every driver calls one before its first
access. Without that, a kernel run on a QEMU built before one of its
devices existed panics *inside the driver*, which reads as a kernel bug
rather than a missing device. That happened once, with the keyboard, on a
machine whose emulator was one build behind.

Once the devices are up, the boot path creates the shell as a task and then
becomes the idle task. It does not exit — there would be nothing to return
to.

---

## Memory

### Physical

`pmm.c` is a bitmap allocator over 4 KB pages, from the end of the kernel
to the top of RAM. One page at a time; there is no buddy allocator and
nothing needs contiguous physical memory. The machine is configured with
64 MB (`machine.conf`) and the model accepts up to 2 GB.

### Virtual

`vm.c` drives the 68040 MMU: three-level tables, 4 KB pages, with `TC`,
`SRP`, `URP` and the transparent translation registers set up at boot.

The kernel is identity-mapped across all of RAM, so a physical address is
also a kernel address and `uaccess` can hand one straight back. Every user
address space is:

```
0x10000000   program text and data
     ...     unmapped, ~255 MB, for brk and mmap
0x1FF00000   stack, 1 MB, growing down
0x1FFFFFF0   top of stack
0x20000000   end: 256 MB total
```

Supervisor-only on every kernel page, so a program cannot read the kernel.
That is checked rather than asserted — `kernel/vmtest.sh` is fifteen
attempts by a program to touch something it should not, and it exists
because **the 68040 has no S bit on a table descriptor**. A supervisor-only
region carries the bit on every one of its *page* descriptors; there is no
way to mark a subtree. Get that wrong and nothing fails loudly, the kernel
is simply readable from user mode.

**The caches are ON, and the page tables are not cached.** Both of the
68040's caches are enabled at the end of `vm_init` — `cinva` first,
because they come out of reset with undefined tags, then
`CACR = 0x80008000` (bit 31 data, bit 15 instruction). Device registers
and the framebuffer are non-cachable through `DTT0`/`DTT1`, and RAM is
copyback.

Translation tables are marked NON-CACHABLE, and that is not tidiness.
The 68040's table search reads descriptors from memory directly and
writes the `U` and `M` bits back the same way; it does not go through
the data cache and does not snoop it. With tables cached, the MMU walks
descriptors the kernel has only written into the cache, and a writeback
of an older line silently undoes bits the hardware has just set — for
`M`, that is a modified page evicted as clean. Linux/m68k marks
page-table pages non-cachable for exactly this reason. Tables are marked
as they are allocated; the ones built before the map exists are swept
once, from the root, before the caches come on.

**None of that can be observed here.** QEMU has no cache model: it takes
the `CACR` write, ignores the `CM` bits and decodes `cinv` and `cpush` as
no-ops, so a correct setup and a broken one are identical under the
emulator and there is no speedup to measure either. The boot line
reporting `CACR` back is the only evidence available. It is written for
real hardware and is correct by inspection — which is why the conditions
that must hold before it are listed where it is switched on.

Three more things about this MMU that cost real time:

**`movec` does not flush the translation cache.** Every write to `TC`,
`SRP`, `URP` or a TT register, and every change to a descriptor, must be
followed by `pflusha` — on real hardware and under QEMU alike, whose
`movec` helper does no flushing whatsoever. A missing flush does not fail
at the flush. It fails later, on an unrelated access that happened to be
cached.

**A fault taken while pushing a fault frame is not an exception.** It is
`cpu_abort("DOUBLE MMU FAULT")` and QEMU exits with no output at all. The
supervisor stack must be mapped and writable before the MMU comes on, and a
task's kernel stack must be mapped before anything can trap on it. Silent
disappearance is the signature.

**Never use indirect page descriptors (PDT=10).** QEMU resolves the
indirection and then writes the used and modified bits back over the
*indirect pointer*, corrupting the page table on first access.

### Shared pages

A page can belong to more than one address space. `pmm.c` keeps a
reference count beside its bitmap -- the number of holders *beyond the
first*, so a page nobody shares frees exactly as it always did -- and
two things use it:

- **`textcache.c`**, which holds one copy of any page of a file mapped
  privately and read-only. That is how a shared library's text is
  shared: `ld.so` maps libc's text read-only, and every process that
  does so gets the same physical pages. A write, truncate, unlink or
  rename of the file makes the cache forget it (the VFS calls in), and
  when the table is full a page only the cache still holds is given up.
- **`fork`**, which shares every page neither side can write instead of
  copying it.

Nothing in the fault path knows about any of this, and nothing needs
to: the only way a page becomes writable is `vm_protect()`, and that
gives the caller a private copy first when anyone else holds the page.
Copy-on-write, done eagerly, at the one moment write can be granted.

### Reaching into a program

A system call that takes a pointer cannot dereference it. The address
belongs to another address space, and it may not be mapped, may be
read-only, or may cross a page boundary into a different physical page.

`uaccess.c` walks the tables in software, one page-sized chunk at a time,
following `current->as`. That last detail is the whole of a bug that took a
while: `exec` used to set a global address space around a program's entire
run, which worked while the program ran *inside* the spawning call. The
moment a program became a task of its own, nothing set it, every user
pointer looked like a kernel pointer, and the first `write()` handed the
terminal an address belonging to a different address space.

### Demand paging and swap

An access fault is first offered to `vm_fault()`, and because the 68040
pushes the address of the *faulting instruction*, returning from the
fault re-runs it -- which is all demand paging needs from the processor.
A page can be absent in three ways, each an invalid descriptor the fault
path understands: **lazy** (made, zeroed, on first touch -- stacks, the
heap, anonymous `mmap`), **swapped out** (in the swap file, its slot in
the descriptor), or present but **copy-on-write** after a `fork`. The SSW
says nothing about *why* a fault happened, so `vm_fault()` walks the
tables to find out.

**Swap** is a file: `swapon /swap` and `swapoff /swap`. Eviction is the
clock algorithm over the MMU's used bits; it happens only where a page
is about to go to a program, and only takes pages with one holder -- so a
page a sleeping `read()` holds the physical address of, which the
system call pins with a reference, stays put. An address space's memory
is given back when its process exits, not when it is reaped: a zombie
used to hold every page, and then every swap slot, until its parent
waited.

---

## Tasks

A task is a kernel stack and an address space. `struct task` holds a pid, a
state, the saved kernel stack pointer, the address space, its descriptors,
signal masks, its session and process group, its nice value and the
job-control bookkeeping. There are 64 of them, statically allocated, and a
kernel stack is four pages with a guard page below it.

```
TASK_UNUSED   free slot
TASK_READY    wants the processor
TASK_RUNNING  has it
TASK_BLOCKED  waiting for something
TASK_STOPPED  ctrl-Z; will not run until continued
TASK_ZOMBIE   finished, waiting to be reaped
```

**The context switch is a stack-pointer swap and nothing else.**
`switch_context(u32 *save_sp, u32 new_sp)` in `taskasm.s` pushes the
callee-saved registers, stores the stack pointer through the first
argument, loads the second, pops, and returns — into a different task. Each
task's state lives on its own kernel stack. There is no register save area
in `struct task` and there does not need to be.

A new task gets a *fabricated* stack, built by `build_stack()` to look
exactly as though it had been switched away from: an exception frame, the
register set, a return address of `task_entry`, the user stack pointer.
Starting a task and resuming one are then the same operation, which is what
makes `switch_context` the only switch in the system.

### Threads

A thread is a TASK THAT SHARES. `clone(2)` with CLONE_VM, CLONE_FS,
CLONE_FILES, CLONE_SIGHAND and CLONE_THREAD gives a task that shares one
address space (reference counted), one descriptor table (`struct
fdtable`, reference counted), one working directory, one set of signal
dispositions, and one process id -- `getpid()` reports the thread group
and `gettid()` the task. The scheduler, the signal code and the system
call gate are unchanged: a thread is scheduled exactly as a process is.

**Everything that waits, waits on a futex.** `futex(2)` is "sleep unless
this word has changed" and "wake whoever is sleeping on it", and every
mutex, condition variable, semaphore, barrier and join in the C library
is built from those two. The point is what does not happen: an
uncontended lock is one `cas.l` and no system call at all. A futex here
is identified by its address space and address, which is exact because
nothing shares memory between address spaces.

The C library's pthread layer is described in
[`libc/README.md`](libc/README.md). Two things about it are properties
of the machine rather than of the library: **there is no thread
register**, so `__thread` does not work and `pthread_self()` finds a
thread by the stack it is standing on, and `errno` is a call into the
thread's own descriptor for the same reason.

exit_group(2) ends every thread, which is what a C library's `exit()`
calls; `execve` ends every thread but the one calling it, as POSIX says.
A thread is not a child: `wait()` will not collect one, and a thread's
death is told to whoever is joining it by the kernel zeroing a word and
waking the futex on it.

### Scheduling

Round robin over the ready tasks, where **`nice` sets the length of a turn
and not whether there is one**: a step of nice is about a quarter more or
less time, as Linux weighs it, so two busy tasks ten apart divide the
processor about five to one and nothing is ever starved. The idle task is
*excluded from the scan* rather than merely ranked below everything,
because an idle task competing on equal terms takes every other turn from
whatever is actually working.

`getpriority` and `setpriority` take a process, a process group or
everything; `nice(1)` is the utility over them.

**Sessions and process groups.** Every task belongs to a process group and
a session, inherited across `fork` and `exec`. `setsid` starts a new
session for a task that does not already lead a group -- which is why
`setsid(1)` forks first -- and `setpgid` may only move a task within its
own session. The terminal has a foreground process group: that is what
ctrl-C reaches, and a background task that reads gets `SIGTTIN`.

**Preemption happens only on the way back to user mode.** That is the
single most important property of this kernel, because it is what lets it
have no locking at all: the kernel can only be entered by one task at a
time, since a task inside a system call cannot be preempted out of it.

The cost is real and must be respected. A *kernel* task — the shell is one
— is never preempted and must block or yield. If one ever loops without
doing either, the machine stops.

---

## Waiting

`wait.c` provides wait queues, counting semaphores and mutexes.

The sleep/wakeup race is closed by masking interrupts, not by a lock. A
task about to sleep masks, puts itself on the queue, marks itself blocked,
and calls `schedule()`; the mask is released by the switch away. Without
that, a wakeup arriving in the window between "decided to sleep" and "is
asleep" finds a task it can mark ready — and then the task marks itself
blocked over the top and waits for an event that already happened.

Queues are first-come-first-served. A stack would be one line shorter and
would starve whoever arrived first under load, which is exactly when it
would matter.

`sem_post` wakes *one* waiter, not all. A semaphore hands out one permit
per post, so waking everybody has them all wake, find the count already
taken, and sleep again.

`mutex_lock` panics if the current task already holds the mutex. That is a
deadlock against oneself, it happens when a locking function is called from
one that already locked, and saying so is better than hanging: the machine
stops with a reason instead of just stopping.

One bug here is worth naming because it produced a symptom nowhere near its
cause. `sleep_on_timeout` cleared its wakeup flag *after* sleeping rather
than before, so a flag left set by an earlier wake made the next sleep
return instantly, reporting a wakeup that had already been consumed.

---

## Signals and job control

Signals are Linux's: the same 31 numbers, the old 32-bit `sigaction`
with m68k's field order, `sigprocmask`, `sigpending`, `sigsuspend`,
`pause` and `sigreturn`. The default-action table in `signal.c` is short:
the four stop signals stop, `SIGCONT` continues, `SIGCHLD`, `SIGURG` and
`SIGWINCH` are ignored, and everything else terminates. `SIGKILL` and
`SIGSTOP` cannot be caught, blocked or ignored.

**Delivery is on the way back to user mode, and only there.** Raising a
signal sets a bit. Every way into the kernel, whether a system call, a
device interrupt or an exception, saves the same `struct pt_regs`. The
last thing before returning to a program is `signal_deliver()` with
those registers. So a program making no system calls is still reached,
by the timer interrupt's return, and nothing is ever torn down halfway
through a call.

**A handler runs by rewriting the return.** Every register, the old mask,
the user stack pointer and the FPU state go into a frame on the user
stack. The pc is pointed at the handler and the stack at the frame, and
the handler returns to `__sigreturn_trampoline` in `crt0.s`. That
trampoline's `sigreturn` puts everything back. Only the condition codes
of the saved SR are honoured, so a forged frame cannot return to user
code in supervisor mode.

**An interrupted call is restarted when no handler ran** (after ctrl-Z
and `fg`, for instance), restarted after a handler with `SA_RESTART`,
and otherwise returns `-EINTR`. `pause` and `sigsuspend` always return
`-EINTR` after a handler. Restarting puts the call number back in d0 and
steps the pc back over the trap.

**Kernel tasks take no signals.** They never return to user mode, so a
signal to one could never be acted on, and `kill` of one is refused with
`EPERM`. **An ignored signal is discarded when it is sent**, not left
pending to interrupt every later sleep.

Stopping is a state, not an unwind: a stopped task sits where it was,
and `SIGCONT` resumes it there.

`waitpid` reports a stopped child, not only a zombie — otherwise ctrl-Z
stops a program and the shell waits forever for something that is never
going to finish. It reports the stop once, tracked by `stop_reported`.

**`SIGCONT` goes only to a task that is actually stopped.** `fg` used to
send one unconditionally, so every foreground program began life with a
signal pending, and everything that checks `signal_pending()` before
blocking returned immediately the first time. The visible symptom was that
the first packet of every `ping` was lost — including on invocations where
ARP was already cached, which is what ruled out address resolution and
pointed at signals instead.

The terminal has one foreground pid. A background job that reads gets
nothing rather than stealing the user's keystrokes, which is what makes `&`
safe.

---

## System calls

Forty of them. The convention and the numbers are Linux's; the calls
above 400 are local, because Linux has nothing to match.

| | |
|---|---|
| **Files** | `open` `close` `read` `write` `lseek` `ioctl` `unlink` `rename` `stat` `getdents` `fsync` `sync` `statfs` |
| **Directories** | `mkdir` `rmdir` `chdir` `getcwd` |
| **Tasks** | `exit` `getpid` `kill` `waitpid` `sched_yield` `spawn` (1000) `jobctl` (1001) |
| **Time** | `time` `stime` `times` `nanosleep` |
| **System** | `uname` `sysinfo` `reboot` |
| **Network** | `socket` `bind` `connect` `listen` `accept` `sendto` `recvfrom` `shutdown` `netctl` (402) |

`fork`, `execve` and `waitpid` are Linux's. `fork` shares the address
space copy-on-write, so fork-then-exec copies almost nothing. `spawn` is
still here, fork and exec in one call: a path and an argument vector,
and a new task.
`/bin/sh` is the kernel's own shell built as a program, and it is what
`sh -c` and `system()` run.

`jobctl` is what `fg`, `bg`, `jobs` and `ps` are built on. `netctl` is what
`ifconfig`, `ping` and `netstat` are built on — the operations that
configure an interface or send one echo request, which have no socket to
hang off.

`HZ` lives in `uapi.h`, not `timer.h`. `times()` returns ticks, a tick
count means nothing without the rate, so the rate is part of the ABI. Linux
answers the same question with `sysconf(_SC_CLK_TCK)`.

---

## Time zones

The clock keeps UTC and **`TZ` says how to turn that into a local
time**. picolibc's `tzset` reads it, and the shell sets `TZ=UTC0` by
default, so a machine that has not been told where it is does not
guess.

There is no zoneinfo database and no need of one: **a POSIX TZ string
carries its own rules**, which is what the format is for.
`export TZ=MST7MDT,M3.2.0,M11.1.0` in `/etc/rc` is a machine in
Colorado, daylight saving included -- an offset, a summer offset, and
the two dates it changes on.

The shell's own `date` prints UTC and says so. Local time is what
programs show, because that is where `TZ` lives.

## Files

**The disk is interrupt-driven**: a task waiting for a sector
sleeps, and the machine runs something else. That made a new thing
possible -- a task asleep in the MIDDLE of a filesystem operation -- so
every call from the VFS into the filesystem holds a lock (`vfs.c`),
recursive, taken only for the filesystem's own files and calls, never a
pipe's or a terminal's. The disk has its own lock beneath it; swap I/O
takes that one alone, and the disk never takes the filesystem's, so the
two cannot deadlock. At boot, before there is anything to switch to, the
disk polls.

### The devices

| | |
|---|---|
| `/dev/console` | the terminal: its sources and sinks, below |
| `/dev/ttyS0` | the serial port itself |
| `/dev/fbcon` `/dev/vcsa` | the framebuffer console, and its character buffer |
| `/dev/fb0` | the framebuffer, by ioctl or `mmap` |
| `/dev/kbd0` | the keyboard |
| `/dev/hda` | the disk |
| `/dev/nvram` | the M48T59's 8176 bytes of battery-backed RAM; `/bin/nvram` keeps settings there |
| `/dev/null` `/dev/zero` `/dev/full` | as everywhere else |
| `/dev/random` `/dev/urandom` | the generator below; `urandom` never waits, `random` waits until the pool is ready |
| `/dev/klog` | what the kernel has said; a read drains it, and `klogd` copies it into `/var/log/syslog` |
| `/dev/ptmx` `/dev/pts/N` | pseudo-terminals: open the first to get a master, and the second is the terminal at the other end |

**`ls /dev` does not work.** /dev is a name lookup rather than a
directory: `resolve_dev()` turns `/dev/<rest>` into a device of that
name, which is also how `pts/0` is a device whose name contains a
slash. Making it listable is a VFS change and is on the list.

### Where "/" is

Each task has a root directory as well as a working directory, so
`chroot()` works: absolute paths start there, `..` in it stays in it, and
an open directory's path is computed up to it. The working directory does
not move when the root does, as on Linux.

### File times

ext2 records a modification, a change and a last-access time to the second,
and `utimensat`/`futimens` set them, which is what `touch` and `make` need.
Dates run to 2106, not 2038: this kernel's `time_t` is an unsigned 32-bit
count and ext2's is a signed one, so a date past 2038-01-19 is stored as the
same 32 bits with the inode's spare "extra" word marking the later epoch --
which is how ext4 spells it, and what makes Linux read back the date this
kernel meant. A
file that is open and has unwritten metadata is flushed before its time is
set, so that closing it afterwards does not stamp over what was asked for.

`vfs.c` holds mounts, path resolution and open files. A descriptor is a
small integer indexing a per-task table of 64; the table entries point at
refcounted open-file objects, so `dup`, `dup2` and `dup3` cost nothing and
two tasks can share one offset. Descriptors close on `exec` when they are
marked `FD_CLOEXEC`.

Descriptors are inherited by both `task_create()` and `exec`. That they are
inherited by *both* is a bug's worth of experience: a shell created without
them ran perfectly and silently, because it had no stdout, and that reads
as a broken context switch for a surprisingly long time.

### ext2

`fs/ext2.c`, about 3,500 lines, and the filesystem the machine keeps itself
on. Read and write, create and delete, subdirectories, names that are just
bytes, modes and ownership, sparse files, and a consistency check of its
own.

ext2 was chosen so the host can read, write and above all **check** the disk
image with e2fsprogs -- no loop device, no root -- which is what makes
`make write` a one-liner and what makes the test suites able to verify the
guest's writes with the *host's* tools rather than by reading them back with
the same code that wrote them. `e2fsck` is the strongest thing any test here
can say: it recomputes every link count, every bitmap and every directory's
`.` and `..` from the disk alone.

The volume is made with 4 KB blocks (so twelve direct pointers and one
indirect block reach 4 MB, and the boot ROM needs no double indirection),
256-byte inodes (room for the extra word that carries a post-2038 date), and
without `dir_index` -- a hashed directory is readable by a driver that
cannot maintain the hash, but writing into one would leave an index that no
longer finds the names underneath it, so the driver refuses to mount a
volume that has the feature.

Three structural facts it is easy to get wrong:

**A directory entry's `rec_len` is the whole slot**, which may be larger
than the name in it, and a deleted entry is absorbed into the one before
it. Walking by a fixed step works on a fresh directory and desynchronises
on the first deletion.

**`i_blocks` is in 512-byte units**, always, whatever the block size is.

**A zero block pointer is a hole**, which reads as zeroes. It is not an
error and must not be allocated on the read path, or reading a sparse file
fills the disk.

A file unlinked while a program still has it open cannot be freed and is
reachable from no directory, so the superblock keeps the head of a list
threaded through the inodes' own `i_dtime` fields. Mounting walks it and
frees what is on it, so a machine that stopped with a deleted file open
leaks nothing.

### FAT16

`fs/fat16.c` is still here and still registered: the mount probes ext2
first and falls back to it, because a disk from a machine that has never
heard of this one is a FAT disk. Read and write, VFAT long names in UTF-8,
one partition. Two structural facts of ITS format, kept because they are
the sort of thing that is rediscovered painfully: a FAT16 root directory is
a fixed run of sectors that cannot grow while every other directory is a
cluster chain, and `.` and `..` are the only record of a directory's
parent anywhere on the volume.

The startup script is `/etc/rc` because `rc.local` was not a valid 8.3 name
when the disk was FAT and the name was chosen. Nothing limits a name now;
the name stayed.

---

## Randomness

`random.c` is a cryptographic generator, built the way Linux's has been
since 5.17 and cut down to what this machine has.

Everything goes into one BLAKE2s-256 state, the pool, which never outputs
directly: at boot the real-time clock, the ethernet address, the tick and
where the kernel landed; after that **the timing of every interrupt** --
which one it was, the tick, and how far the MFP's timer had counted into
the tick when it arrived, at 12288 Hz -- and anything written to
`/dev/random`.

Output is ChaCha20 keyed from the pool, with **fast key erasure**: every
request draws its next key from the stream before anything is returned, so
what came out before cannot be recomputed from the state after.

How much is credited decides when `getrandom()` stops waiting, and it was
measured rather than assumed: a timer tick's own counter reading is the
same value 85% of the time, so a tick is credited an eighth of a bit and
any other interrupt one bit. The pool is ready at 128 bits -- about ten
seconds on an idle machine, much sooner with anything happening.
`/dev/urandom` and the kernel's own uses never wait, as on Linux.

`crypto.c` holds the two primitives, with no kernel dependencies, so that
`kernel/cryptotest.sh` can build them for the host and check them against
RFC 7693 and RFC 8439; the same program runs on the machine, which is
big-endian where the host is not.

---

## The terminal

`tty.c` is a terminal, not a driver. `/dev/console` has a list of input
sources and a list of output sinks. `ns16550.c` is a raw serial port
(`/dev/ttyS0`) registered as both; `fbcon.c` is a sink; the keyboard is a
source. `console` lists the sinks and `console NAME on|off` switches one --
`console fbcon off` takes output off the screen, `console ttyS0 off` off
the serial line. Turning off the last one is refused.

Echo goes to the *sinks*, never back to the source. Otherwise output on the
screen means typing blind.

**Console output is not exclusive and must not become one.** The test
harnesses drive the machine over serial with `-display none`, and QEMU
delivers no keyboard input without a display. An exclusive console breaks
every test in the tree.

The line discipline knows how to assemble a line with erase and kill, and
nothing more. Everything else — history, ctrl-R, word motion — is in the
shell, which is where bash's is. Putting ctrl-R in a kernel would be
putting a shell's memory inside the machine.

**Input is interrupt-driven** (task 22). The serial port and the
keyboard raise their interrupts through the MFP, and the handler drains
the chip into a 256-byte ring, acting on ctrl-C and ctrl-Z as it goes.
The MFP sees EDGES, so a handler must leave its chip quiet -- a line left
high makes no new edge and the device is never heard from again. And a
full ring does not drop: it stops draining and leaves the rest in the
chip, whose full FIFO is what makes the sender wait. The first version
dropped, and a burst of typed-ahead input lost its middle.

`poll_char()` still masks against the timer, because the tick polls the
sources that have no interrupt (the console's own replies). If it takes a
character between a reader checking the pushback slot and that reader
taking one from the device, the two come out in the wrong order: a line
typed `SHELL` arrives as `SHLEL`, rarely enough to be baffling.

**The line editor moves the cursor with `\r` and `\b` only.** That was
forced when `fbcon.c` understood nothing else; it is a VT102 now, and the
rule stays because it works on any terminal. The redraw is deliberately
minimal: a character typed at the end of a line echoes one character,
because on the framebuffer each one is 128 pixels drawn individually and
redrawing a whole line per keystroke is visibly slow.

---

## Pseudo-terminals

A pty is a pair of devices and a line discipline between them. What a
program writes to the MASTER arrives at the SLAVE as though it had been
typed; what the program on the slave writes comes back out of the master
as though it had been displayed. The slave is a terminal in every way a
program can ask: `isatty()` says so, it has termios settings, a window
size, and a foreground process group whose members are what ctrl-C on
that pty reaches.

`/dev/ptmx` allocates a pair and gives back the master; the slave is
`/dev/pts/N`, and N is what the master's `TIOCGPTN` says -- which is
what `ptsname(3)` asks. `openpty` and `forkpty` are in the C library.

The discipline is the terminal's, not the console's: canonical mode
assembling lines with erase and kill, raw mode delivering characters as
they arrive, ECHO going to the MASTER (which is where the person is),
ICRNL and ONLCR, and ISIG turning the interrupt, quit and suspend
characters into signals for the foreground group. Closing either end is
visible at the other: the slave reads end of file, and the program on
it gets SIGHUP.

Eight pairs, and they are given back -- which `kernel/ptytest.sh`
checks by taking every one, closing them, and taking them all again.

## Users

A task has a real, effective and saved user id and the same three group
ids. They are inherited by `fork` and kept across `exec` -- except where
the file carries the set-user-id bit, below -- and moved by `setuid`,
`setgid`, `setreuid`, `setregid`, `setresuid` and `setresgid` under the
rules POSIX gives: root may become anybody, and anybody else may only
move between the identities they already hold, which is exactly enough to
drop a privilege and take it back.

A change reaches **every task in the thread group**, not just the one
that asked. Credentials belong to a process; a thread still holding the
old uid after its siblings changed would be a hole.

`/etc/passwd` and `/etc/group` are ordinary files in the traditional
format. The C library reads them (`getpwnam`, `getpwuid`, `getpwent`);
the shell reads `/etc/passwd` itself, with `open` and `read`, because the
shell may not call the C library -- which leaves two independent
implementations of the same lookup, and a test that checks they agree.

### Logging in

The console and `sshd` both go through **`/bin/login`**: it asks for a
name and a password, checks the password against `/etc/shadow`, and on
success drops to that person -- `setgroups`, then `setgid`, then
`setuid`, **in that order**, because a `setuid` done first gives away
the privilege the other two need and leaves a process holding groups it
has no right to. Each of the three has to have *worked*; a failed drop
that is not noticed is a root shell handed out by accident.

It then `chdir`s to the home directory from `/etc/passwd`, sets `HOME`,
`SHELL`, `USER` and `LOGNAME` from the same line, and execs the shell
named there with a leading `-` on `argv[0]` -- which is how a shell is
told it is a login shell and so reads `~/.profile`.

The shell spawns `login` again when a session ends, so the console
returns to a prompt rather than to a root shell. **If `/bin/login` is
missing or will not exec**, the shell says so and falls back to the
built-in root session: a disk with no login program has to be
recoverable.

Passwords are hashed with **`$6$` SHA-512 crypt**, salt from
`/dev/urandom`, and live in `/etc/shadow`, which is root-only.
`/etc/passwd` carries `x` in that field and stays world-readable,
because every `getpwuid` reads it to turn a number into a name and a
hash everybody can read is a hash everybody can attack offline.

`passwd`, `su` and `sudo` are installed **4755**: they need root to
read the shadow file or to become somebody, and the set-user-id bit on
exec is what gives it to them. `sudo` consults `/etc/sudoers`; the
group it trusts is `wheel`. `useradd` and `userdel` edit the four files
together, by writing a new copy and renaming it over the old one, so an
interrupted edit leaves the previous file rather than half of a new one.

### What is enforced

Every path a system call takes is checked (`perm_ok`, `walk_ok` in
`kernel/vfs.c`):

- **owner, then group, then other -- first match wins.** Not "the most
  permissive that applies": a file mode `0607` denies its owner the
  write that `other` is given, and that is the POSIX rule rather than an
  accident. Group membership counts the effective gid *and* the
  supplementary groups `login` set from `/etc/group`.
- **every directory in a path needs its search bit**, which is what
  makes a mode `0700` home directory private even when a file inside it
  is world-readable.
- **root bypasses all of it**, with one exception that matters: root may
  execute a file only if *some* execute bit is set. Otherwise every data
  file on the disk would be a program to root.
- **set-user-id and set-group-id on exec** are honoured (`kernel/exec.c`),
  and `chown` **clears both**, because otherwise giving a file away
  would hand over the privilege with it.
- `chown` to another user is root's alone; changing only the group is
  allowed to the owner, for a group they belong to.

`chmod`, `chown`, `fchmod` and `fchown` are real, and `ls -l` prints
what they set. `kernel/usertest.sh` used to end with a check that read
root's file as an ordinary user and *passed*, so that nothing could be
read as evidence of a protection that did not exist; it now checks that
the read is refused.

### Links

`link`, `symlink` and `readlink` work, and `lstat` reports the link
rather than what it points at. ext2's **fast symlinks** keep a target of
60 bytes or less in the inode's block pointers -- the 60 bytes that
would otherwise hold `i_block` -- with no data block at all; longer ones get a block, which is a different write path and so has
its own tests.

Following happens in the path walk, **iteratively**, with a depth limit
of 40 and `ELOOP` past it -- a pair of links pointing at each other is
otherwise not a crash but a machine that has stopped, with the
filesystem lock held. The scratch buffers are static rather than
automatic: two `PATH_MAX` paths and a target is 3 KB against a 16 KB
kernel stack shared with everything else the call is doing, and the
recursive first version overflowed it into a double fault. Static is
safe here only because the filesystem lock is held throughout.

`unlink`, `rename` and `lstat` need the *other* resolution -- the one
that stops at the link -- so the walk exists in both forms.

## The log

The kernel keeps everything it prints in a ring and writes to no file:
its first messages exist before there is a disk driver, and a panic has
to work with the filesystem in whatever state it is in. The ring is
**`/dev/klog`**, and a read of it blocks until there is something and
DRAINS what it takes -- `/proc/kmsg`'s rule rather than `/dev/kmsg`'s,
because the only reader is the one whose job is to put the bytes
somewhere they can be read repeatedly.

That reader is **`klogd`**, started from `/etc/rc`: asleep until the
kernel speaks, appending to **`/var/log/syslog`** with a timestamp and
the machine's name on each line. `syslog(3)` appends to the same file,
so a program's messages and the kernel's are one log in the order they
happened. **`dmesg`** reads the ring directly, which is what a machine
with no klogd running wants.

## Doing something later

`cron` (sbase's) reads `/etc/crontab`, keeps its pid in
`/var/run/crond.pid`, and says what it is doing through `syslog(3)` --
which appends to `/var/log/syslog`, the file klogd writes the kernel's
messages to, so the machine has ONE log rather than one per source.

It is the first thing here that happens because the CLOCK said so
rather than because somebody typed something, which makes it as much a
test of the clock and of a long-lived background task as of cron.

## The network

`kernel/net/`: ARP, IP, ICMP, UDP, DHCP, TCP and a socket layer, over the
SMC LAN91C111. Written out rather than imported — the design notes said to
bring lwIP in at TCP, and that was right until the layers below it existed.
lwIP is not a TCP, it is a whole stack with its own ARP, its own IP and its
own idea of what an interface is, so adopting it now would mean discarding
all of that rather than slotting a layer on top. The socket layer is what
keeps the option open.

### Getting frames off the card

**The LAN91C111 allocates transmit buffers from the same page pool that
holds arriving frames**, so a receiver that is never drained stops the
machine being able to *send*. On QEMU's user-mode NAT almost nothing
arrives unasked and this never shows. On a real LAN, broadcast traffic
fills the card within seconds and every transmit fails with ENOMEM.

So `timer_tick()` calls `net_drain()` unconditionally to move frames off
the card into a ring, and `net_poll()` does the protocol work in ordinary
kernel context. **The protocol code must not run from the interrupt** —
that is what keeps the stack free of locking.

Transmit raises the interrupt mask for the length of one send, because
receive and transmit both bank-switch the chip and share its pointer
register, so the tick's drain landing mid-send would leave the chip
pointing somewhere else.

### TCP

The state machine of RFC 793, both opens, an orderly close on both sides,
and then the parts that a stack on a real network cannot do without:

- **Out-of-order reassembly.** Segments arriving ahead of a gap are held
  rather than dropped. Dropping is legal — a receiver may discard anything
  it does not want — and it turns one lost packet into a stall for a whole
  round trip, because the sender has to time out before anything else can
  be accepted. The buffers are a shared pool rather than per connection: a
  reassembly queue is only occupied during a loss, so giving every
  connection its own reserves memory for a situation that is rare on all of
  them at once.
- **Congestion control**, RFC 5681: slow start, congestion avoidance, fast
  retransmit, fast recovery. The peer's window says what it can *receive*;
  the congestion window is this end's estimate of what the *path* can
  carry, and a sender may use the smaller of the two and nothing else.
- **RTT estimation and a computed RTO**, RFC 6298, with Karn's algorithm.
  The retransmission timeout used to be a constant that doubled; measuring
  it means recovering from a loss in about one round trip instead of half a
  second.
- **Delayed acknowledgements.**
- **Initial sequence numbers that cannot be guessed**, RFC 6528. It was
  `jiffies * 7919`, which is to say a multiplication of a number that
  increases by one every ten milliseconds — guessable by anyone who knows
  roughly when a connection was made, and an off-path attacker who can
  guess it can inject data into somebody else's connection without ever
  seeing it. `random.c` is xorshift32 seeded from the clock, the tick, the
  MAC address and where the kernel landed. **It is explicitly not a
  cryptographic generator** and says so at the top of the file.

Most of that list was, until recently, a list of things this stack
deliberately did *not* do, on the grounds that a machine talking to its own
LAN is not where the internet's congestion is decided. That reasoning held
exactly as long as the only network was QEMU's NAT.

And the negotiated options: **window scaling, timestamps with PAWS, and
SACK** (RFC 7323, RFC 2018), on 64 KB send and 128 KB receive buffers;
**keepalives** with Linux's `TCP_KEEP*` options; and a **60-second
`TIME_WAIT`** that a reset does not cut short.

Still absent, each on purpose: path MTU discovery, and Nagle -- a human
on the other end is not where the forty-byte-header problem gets
solved, and coalescing would make an interactive session worse.

### On a real LAN

`tools/qemu-net.sh` decides at runtime: an existing bridge if there is one,
otherwise macvtap on a wired interface, otherwise slirp. It has to be
runtime, because **Wi-Fi cannot bridge** — an 802.11 station may only use
its own MAC as the source of frames it sends, so bridging and macvtap are
both impossible on a wireless link. One development machine here is
wireless and the other is wired.

The test suites always use slirp, deliberately: a test that depends on the
building's network is not a test.

### Logging in over it

**Dropbear** (`ports/dropbear`) is the ssh server, the client, `scp`
and `dropbearkey`, and **rsync** is beside it. A real OpenSSH client on
another machine authenticates into the Sage040 by public key and runs
commands; `scp` copies files in both directions; `rsync` runs over that
same ssh. `dropbearkey` generates the Ed25519 host key **on the
68040**.

Dropbear rather than OpenSSH, and the reasoning is at the head of
`ports/dropbear/build.sh`: OpenSSH separates privilege by forking a
child, setuid-ing it to a dedicated account and chrooting it into an
empty directory. That rested on a filesystem enforcing ownership,
which this one did not do at the time the choice was made; it does
now, but Dropbear was also written for machines this size and carries
its own crypto, so it need not agree with OpenSSL about anything. The
protocol is the same protocol.

**Password authentication is on**, and was not. Checking a password
means `crypt(3)` against a hash in `/etc/shadow`, and the machine had
neither; it has both, so ssh asks the same question the console asks
and gets its answer from the same file. Dropbear reads the hash
through `getpwnam`'s `pw_passwd`, so the C library hands it the shadow
hash rather than the `x` that is in `/etc/passwd`. Public keys work
too, and are still the better authentication.

**`rsync -a` asks for ownership to be preserved** and reports
`chown ... failed`. `-rlt` is the flag set that matches what this
system enforces.

Two things are worth knowing. A modern OpenSSH client needs `scp -O`
to use the old protocol at all, Dropbear having no sftp-server. And
**scp's port flag is `-P`**; `-p` means "preserve modification times",
so passing the port as `-p` turns it into an extra source file and
produces `No such file or directory` -- which reads exactly like a
broken scp, and was read that way for a while.

---

## Programs

A program is an ELF executable with no extension. **`exec.c` decides what
is executable from the file's first four bytes**, because the disk had no
permission bit — do not "tidy" this by adding `.EXE` or by matching on
names.

`lib/` is what a program written for this system links against: `crt0.s`,
`ulib.c`, `user.ld`. `system/` is what the system ships, installed into
`/bin`: `ifconfig`, `ping`, `netstat`, `host`, `ntpdate`, `shutdown`,
`env`, `stty`, `resize`, `fsck`, `df`, `id`, `klogd`, `dmesg`,
`swapon`, `swapoff`, `nvram`, `irqs` and `sh`. `apps/` is everything
else, installed at the root: `cube`, `fbtest`, `fbmap`, `hello`,
`fetch`, `httpd`, and the test programs.

**A program keeps its own name.** It used to be upper-cased on the way
onto the disk, because FAT16 had no lower case in a short name and
`winchtest` became `WINCHTES`; on ext2 a name is just bytes.

None of them is privileged. A system program can do only what the system
call interface allows, which is the point of it being a program: the ones
that cannot be written that way are the argument for a system call that is
missing.

`ports/` holds programs written by other people. No source from any of
them is copied into this tree: each port fetches its own at a pinned
version, applies whatever patches are kept beside it, and builds
against picolibc. The reasoning for each is at the head of its
`build.sh`, and several have a README as well.

| | |
|---|---|
| the tools | bash, sbase (98 utilities), sed, grep, awk |
| editors | uEmacs, vi, and `less` for reading |
| the terminal | ncurses 6.5 and a terminfo database of seventeen terminals at `/usr/share/terminfo` |
| the network | Dropbear (ssh, scp, dropbearkey) and rsync |
| languages | CPython 3.14.7, with 90 built-in modules |
| libraries | OpenSSL, SQLite, bzip2, xz, zstd, readline, libffi, libiconv, gettext's runtime |
| the toolchain | binutils 2.45 and gcc 15.2.0, which run ON the machine -- see [`toolchain.md`](toolchain.md) |

**Every terminfo entry begins with a lower-case letter.** terminfo
stores one directory per first letter, and the database has entries
(`Eterm`, `emu`) that differ only in case; the build refuses a set with
two such names. That was forced when the disk was FAT and could not
tell them apart. ext2 can, and the constraint has not been lifted
because nothing has needed it to be.

Four terminals are compiled into the library as fallbacks -- vt102,
vt100, dumb, unknown -- so a program works on a disk with no database
at all.

`ulib` is a thin wrapper over the system calls plus the handful of string
and output helpers that every program needs, and `lib/malloc.c`, a
stand-in allocator. Anything written the way programs are written
elsewhere is built against **picolibc** instead (`libc/`), whose Linux
layer runs unchanged because the system calls are Linux/m68k's.

A picolibc program can be **dynamically linked**: `PT_INTERP` names
`/lib/ld.so` (`ldso/`), which the kernel loads beside the program and
starts first; it loads `/lib/libc.so` and any other `DT_NEEDED`
library, binds every symbol before `main`, and jumps to the program.
The editors in `ports/` are built that way. `programmer-guide.md` has
the details, and why each is as it is.

`crt0.s` reads `argc` and `argv` at `4(%sp)` and `8(%sp)`, not 0 and 4 —
the kernel enters a program with `jsr`, which pushes a return address
first. Getting that wrong gives a plausible-looking garbage `argc` and a
bus error a few instructions later.

---

## The shell

`shell.c`, about 2,100 lines, running as a task like anything else.

- Environment variables, `export`, `unset`, and inheritance by spawned
  programs
- `PATH`, searched by `spawn_on_path()`, and **`/bin:/usr/bin:.`** by
  default -- set before `/etc/rc` runs. The first directory that has
  the name wins, and the working directory is searched **last**, so a
  program dropped in it cannot quietly replace a system one. `./name`
  runs the one here whatever PATH says, a name with a slash being a
  path and not a search. `execvp` in `lib/ulib` and picolibc's both
  walk it the same way, so a program that spawns by name agrees with
  the shell.
- `$VAR`, `${VAR}`, `$$` and `$?` expansion
- Shell scripts, run by path or with `source`; `/etc/rc` at startup
- Job control: `&`, `jobs`, `fg`, `bg`, `ps`, `kill`, ctrl-Z, ctrl-C

The builtins are `.` `bg` `cat` `cd` `clear` `console` `cp` `date` `df`
`echo` `export` `fg` `free` `halt` `hd` `help` `history` `jobs` `kill`
`ls` `mkdir` `mv` `ps` `pwd` `rm` `rmdir` `set` `source` `stat` `sync`
`test` `uname` `unset` `uptime`.

`set` is a builtin and `env` deliberately is not: `set` changes the
shell's own state, which only the shell can do, while `env` only reports
what it was given and so proves that inheritance works.

The line editor in `edit.c` is emacs-shaped: ctrl-A, ctrl-E, ctrl-B,
ctrl-F, ctrl-K, ctrl-U, ctrl-W, ctrl-D, ctrl-L, the arrow keys, history
with ctrl-P and ctrl-N, and incremental search with ctrl-R and ctrl-S. It
turns canonical mode off with a Linux-shaped `TCSETS` and does the work
above the system call boundary.

`shutdown` works by telling the keyboard controller to pull the reset line
— the 8042's spare output pin, wired to RESET on the IBM PC because there
was nowhere else to put it. With `-no-reboot` a guest reset ends QEMU, so
the machine stopping and the emulator exiting are the same event. **Every
QEMU invocation in this tree needs `-no-reboot`**, or `shutdown` restarts
the machine instead of ending it.

---

## Testing

Everything here is tested, and `make test` runs the lot: the twelve
bare-metal device tests, the crypto vectors on the host, and twenty-one
scripted suites that each boot the machine and drive it over its serial
line.

| | | |
|---|---|---|
| `tests/` | 12 programs | the devices: CPU and FPU, UART, ATA, MFP and its timers, MMU, SM501, RTC, keyboard |
| `kernel/cryptotest.sh` | 4 | ChaCha20 and BLAKE2s against the RFCs, built for the host |
| `kernel/fstest.sh` | 65 | the filesystem, names of any shape included, and a file past what one indirect block reaches -- verified with the host's debugfs and e2fsck |
| `kernel/apitest.sh` | 368 | the system call surface a ported program expects |
| `kernel/edittest.sh` | 41 | the line editor, history, job control, command lists, scripts, shutdown |
| `kernel/vmtest.sh` | 18 | what a program cannot touch |
| `kernel/pagetest.sh` | 51 | demand paging, copy-on-write, swap, and running out of memory |
| `kernel/nettest.sh` | 19 | ARP, DHCP, ICMP and TCP against the host; the SYN's options decoded there |
| `kernel/lotest.sh` | 22 | the loopback interface, and 127/8 frames forged onto the wire |
| `kernel/dnstest.sh` | 57 | both resolvers and `ntpdate`, against servers on the host |
| `kernel/tcptest.sh` | 24 | TCP's options, loss, keepalives and TIME_WAIT |
| `kernel/vttest.sh` | 95 | the VT102 console, checked against screenshots |
| `kernel/devtest.sh` | 36 | interrupts, the filesystem under concurrency, the limits, the NVRAM, `mmap` of the framebuffer |
| `kernel/libctest.sh` | 232 | picolibc and the POSIX layer added to it |
| `kernel/sotest.sh` | 119 | shared libraries, `ld.so`, and the sharing of their pages |
| `kernel/fscktest.sh` | 23 | `fsck`, against seven kinds of damage made on the host, each repaired and then agreed with by e2fsck |
| `kernel/fattest.sh` | 10 | the FAT16 fallback, which is no longer the machine's own filesystem |
| `tools/fsimgtest.sh` | 34 | the host's end of the disk, which every other suite stages its files through |
| `kernel/uemacstest.sh` `kernel/vitest.sh` | 9, 9 | the two editors |
| `kernel/awktest.sh` | 40 | awk's own regression tests, and eleven more against the host's awk |
| `kernel/sedtest.sh` | 21 | sed, against the same sed built for the host |
| `kernel/greptest.sh` | 37 | grep's own 329 pattern cases, and its options against the host's grep |
| `kernel/sbasetest.sh` | 76 | the utilities, against the host's own, and what only the disk can say |
| `kernel/bashtest.sh` | | the shell language against the host's bash, and part of bash's own suite |
| `kernel/threadtest.sh` | 52 | threads: clone, futexes, and the pthread layer, with the lock's own negative control |
| `kernel/ptytest.sh` | 34 | pseudo-terminals, and that the pairs are given back |
| `kernel/curstest.sh` | 28 | terminfo and curses, with the database renamed away as the control |
| `kernel/lesstest.sh` | 12 | less, and a full-screen program on a terminal that cannot address its cursor |
| `kernel/logtest.sh` | 10 | the kernel's log, klogd, and /var/log/syslog |
| `kernel/crontest.sh` | 8 | something the machine does by itself, later |
| `kernel/pytest.sh` | 43 | CPython, against the host's Python's answers to the same questions |

`make bashsuite` runs every one of bash's 83 tests instead of the subset,
which takes hours: one test is minutes of work for a 25 MHz 68040.

The picolibc suites need `make libc` first.

Everything they write goes in `scratch/`, and `make clean` removes the lot.
`hd.img` is not in there: that is the machine's disk, not a build product.

Two things about this that are load-bearing:

**When a test passes, ask what it would fail to catch.** `t3-ata` is the
cautionary example. ATA byte order needs *no* swap for sector data and
*does* need one for `IDENTIFY`, and getting it backwards writes a
byte-swapped image that still passes a write-then-read-back test, because
both directions swap. It went unnoticed for weeks and only surfaced when
the boot ROM tried to load a host-written payload. The test now also checks
a signature planted in the image by the harness — a round trip provably
cannot catch this.

**A harness's sleeps have to happen while sending**, not while building the
script. `edittest.sh` built its session in a block whose sleeps delayed the
file's construction and nothing else, so the whole thing arrived in one
burst. It did not matter until signals became precise: a ctrl-Z "typed two
seconds later" landed before the program existed and went to the shell.

---

## What it is not

Named, so that nobody has to discover them by trying.

**What is executable is still decided by a file's first four bytes**, not
by the execute bit. The mode is checked -- `perm_ok` refuses an exec
without an execute bit -- but what makes a file a *program* rather than a
script or data is its header, which is also how `exec` decides. Two
different questions that a Unix answers with one bit are answered with
two things here.

**No FIFOs and no device nodes on disk.** `mkfifo` and `mknod` answer
`EPERM`. The devices are the ones the kernel makes; a FIFO could live in
the VFS rather than on the disk, and does not yet. Hard links and
symlinks, which used to be in this paragraph, work -- see "Users".

**No `/dev/fd`, and so no process substitution in bash.** `<(...)` needs
either that or a FIFO.

**No `diff`.** sbase has none; GNU diffutils is the obvious port.

**A built-in can be shadowed by a program on purpose.** `df` is the
case: the built-in takes no arguments and prints one fixed report,
which is wrong for a command with `-h`, `-k` and `-i`. It runs
`/bin/df` when there is one and answers itself only when there is not
-- which is what keeps it useful on a disk whose filesystem is the
thing being investigated.

**No `dlopen`.** `ld.so` resolves what a program was linked against and
stops, so a library cannot be opened by name at run time. libffi is built
and works, and Python's `ctypes` still cannot be built, because
`_ctypes.c` wants `<dlfcn.h>`. This is the loader's missing feature, not
the library's.

**No thread-local storage.** The 68040 has no thread pointer register, so
`__thread` compiles to a call to `__m68k_read_tp` that nothing provides.
Giving the system real TLS means PT_TLS in `ld.so`, a per-thread block
and that function in the C library. Software that uses `__thread` for an
optimisation -- bfd does, for one variable -- falls back to a global.

**A fault's own signal cannot be caught.** `SIGSEGV` from an access fault
ends the program: the 68040's access-fault frame cannot be redirected to a
handler in place. Every other signal works, including `SA_SIGINFO` with
Linux/m68k's `siginfo` and `ucontext`, and `sigaltstack`.

**One filesystem, one partition, one network interface.** The static
limits that were constants are larger now -- 64 tasks, 64 descriptors
each, 256 open files -- but these three are structure.

**Swap is a file, one at a time**, and there is no swap cache: a page read
back in gives up its slot, so evicting it again writes it again. When
memory is overcommitted and runs out, whoever faults is killed -- there is
no chosen victim.

A swap cache -- keeping the slot, and using the MMU's `M` bit to DROP a
page nothing had written rather than write it again -- was built and
taken back out; progress.md says what went wrong and what to know before
trying it a second time. The short version is that `M` lives on a
descriptor while a remembered slot belongs to a frame, and that the
kernel writes user pages by physical address, so `read(2)` filling a
buffer sets no `M` bit at all.

**No floating point in the kernel.** Programs may use the FPU; `cube`
does.

`design.md` keeps the list of what is planned, and what each would take.
