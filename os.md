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
**What it is not**, at the end of this document, says where the edges are.

---

## Contents

- [Shape of the thing](#shape-of-the-thing)
- [Booting](#booting)
- [Memory](#memory)
- [Tasks](#tasks)
- [Floating point](#floating-point)
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

**The ROM reads the journal, and never writes it.** It runs before the
kernel, so before anything has replayed the log, and a volume stopped with
committed transactions still in it has its newest metadata there and not at
home -- a KERNEL.ROM replaced just before a power cut would load as the old
one. So when the superblock says `needs_recovery`, the ROM walks the log as
the kernel's recovery does (committed transactions only, revokes honoured)
and builds a map of the blocks it holds newer copies of; every metadata read
goes through the map, and the disk is left exactly as it was for the kernel
to replay. File data needs no map: the journal is ordered mode, so only
metadata is ever in it. It also follows double-indirect blocks now, for the
journal's own inode -- a 16 MB journal is far past a single indirect block.
`kernel/bootjtest.sh` makes that disk on purpose and checks the ROM boots the
new kernel where the home blocks still name the old one.

**A disk made before the journal is converted in place**: `make journal`
(and `make install`, which runs it) adds ext3's journal with `tune2fs -j`,
files untouched -- and refuses while an emulator has the image open or while
the volume is not clean by e2fsck, then checks it again afterwards. A no-op
on a disk that already has one.

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

**A program's code and constants are read-only.** exec loads each
segment into writable pages and then write-protects every page of a
segment the ELF file does not mark writable, so a store into a
program's code or a string literal is a SIGSEGV (`SEGV_ACCERR`), as on
Linux, rather than silently succeeding. The linker scripts --
`lib/user.ld` for ulib programs, `libc/sage040.ld` for static picolibc
ones, ld's own for dynamic ones -- put text and data in separate
segments for that to have anything to act on; the first two used to make
one segment of everything, flagged RWE. A read-only page is
also one `fork` shares without copying.

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
- **`MAP_SHARED` of a file**, whose pages are the text cache's too: every
  mapping of page N of the file, in any process, is the same physical
  page, writable if the mapping is, so a store is seen by all of them at
  once. Such a page is marked in each descriptor (`DESC_SW_SHARED`), so
  fork shares it instead of copying it, mprotect never copies it, and
  reclaim never swaps it -- it belongs to the file. It goes back to the
  file on `msync`, `fsync`, the last unmap and exit, and the cache holds
  the file open meanwhile, so a mapping outlives its descriptor and the
  file's name. `read()` and `write()` agree with it: both write dirty
  shared pages back first, and `write()` reads them in again after.
  **Dirty is conservative**: a page is written back if a writable
  mapping of it has existed, modified or not, because nothing can find
  every descriptor of a shared page to read the MMU's modified bit.

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
[`libc/README.md`](libc/README.md). **There is no thread register**, so
the kernel keeps each task's thread pointer (`tp` in `task.h`), as
Linux/m68k does: `set_thread_area` and clone's `CLONE_SETTLS` set it,
fork keeps it, exec clears it, and `get_thread_area` is how
`__m68k_read_tp` -- which gcc calls for every `__thread` access --
reads it. That makes thread-local storage a system call per function
that uses it, which is fine for `__thread` and too slow for
`pthread_self()`, which still finds a thread by the stack it is
standing on.

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

## Floating point

The MC68040's FPU has the MC68881/MC68882 registers and data types, and
only part of their instruction set in silicon: the four arithmetic
operations, `fsqrt`, `fabs`, `fneg`, compares and moves. The rest --
`fsin`, `fcos`, `ftan` and their inverses and hyperbolics, `fetox`,
`ftwotox`, `ftentox`, `flogn`, `flog2`, `flog10`, `fint`, `fintrz`,
`fmod`, `frem`, `fscale`, `fgetexp`, `fgetman`, `fsincos` and `fmovecr`
(MC68040 User's Manual, Table 9-10) -- raise the *unimplemented
floating-point instruction* exception, and operands the chip cannot take
(denormalised, unnormalised, packed decimal) raise *unsupported data
type*. The kernel carries **Motorola's M68040 Floating-Point Software
Package** (`kernel/fpsp/`, `fpsp/ORIGIN.md` says where it came from and
what was changed) as the handler for both: it computes the result to the
MC68881's precision, puts it in the program's register, and resumes the
program, which cannot tell the difference. So a program may use every
MC68881 instruction, as it could on a 68030 with a 68882.

`kernel/fpspglue.s` is this kernel's side of the package -- what
Motorola's `skeleton.sa` left to each operating system:

- The package reads and writes the program's memory through
  `copy_from_user`/`copy_to_user`, which may sleep on a page fault; a
  task switch in the middle of an emulated instruction is safe, because
  `schedule()` saves the whole FPU -- the state frame included -- in the
  task.
- An arithmetic exception the program has enabled in FPCR (overflow,
  divide by zero, ...) and the package decides is real becomes
  **SIGFPE**, with Linux/m68k's `si_code` -- `FPE_FLTDIV` for divide by
  zero, `FPE_FLTOVF`, `FPE_FLTUND`, `FPE_FLTRES` for inexact, and
  `FPE_FLTINV` for operand error, signalling NaN and BSUN -- and the PC
  of the instruction that took it as `si_addr`. An F-line instruction
  that is not floating point at all becomes **SIGILL**. Both arrive
  through the ordinary signal path, on the 68040's format $3 or $0
  frames.
- An enabled exception is *posted*: the instruction that caused it
  leaves its destination alone, and the NEXT floating-point instruction
  takes it. So a handler that simply returns lets that next instruction
  run, with the destination as it was before -- a signalling NaN in an
  FADD's source does not deliver a NaN; the FPSP writes a quieted one
  only for an FMOVE out to memory or an integer.
- `kstat(KSTAT_FPSP)` counts what the package did: instructions
  completed, operands handled, exceptions reported.

A task's FPU save area gives the state frame 100 bytes, because a real
68040 can FSAVE a 100-byte busy frame when a task is switched out with
an exception pending.

**Under QEMU** the transcendentals are computed by QEMU itself, as it
does for every m68k CPU, and the package is not reached -- unless the
CPU is `-cpu m68040,fpsp-trap=on`, a property of this tree's QEMU patch
that makes it trap exactly as the silicon does. `kernel/fpsptest.sh`
boots both ways and checks every function against the host's libm, and
checks that with the property on it was the package that computed them.
With the property on, QEMU also posts the E1 exceptions -- divide by
zero, operand error, signalling NaN -- and BSUN as the chip does, and the
suite runs each through the package to a SIGFPE with its `si_code`. What
it cannot show is overflow, underflow and inexact from the arithmetic
instructions: their state frame holds an intermediate result softfloat
never produces, so the patched QEMU does not raise them
(`qemu-patch/README.md`).

A ulib program gets these as `<math.h>` (`lib/math.h`, the instructions
themselves); a picolibc program has picolibc's libm.

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

**A program's own faults are signals it can catch.** An access fault
the kernel cannot resolve, an illegal instruction, divide by zero, CHK,
TRAPV, a privileged instruction, `trap #15` -- each becomes SIGSEGV,
SIGBUS, SIGILL, SIGFPE or SIGTRAP with Linux's `si_code` (SEGV_MAPERR or
SEGV_ACCERR, FPE_INTDIV, ILL_ILLOPC, TRAP_BRKPT, ...) and, for an
`SA_SIGINFO` handler, the faulting address in `si_addr`. It is delivered
at once: `_exc_common` returns through `task_ret_to_user` as a system
call does, after `make_format0` in `trap.c` has turned the 68040's
30-word access-fault frame into a four-word one at the same PC -- the
only kind a handler can be sent through. A handler that repairs the
cause and returns (an `mprotect`, say) gets the instruction run again; one
that changes the PC in its `ucontext` moves the program on.
`kernel/faulttest.sh` does seven. A fault cannot be ignored or blocked:
either way it ends the program, printing the registers, exactly as a
fault with no handler does -- as Linux does, since the instruction would
only fault again. A SEGV_ACCERR is a refused access to a resident page;
a `PROT_NONE` page is not resident here and reports SEGV_MAPERR, where
Linux says ACCERR.

**On a real 68040 a store that faults has already happened** as far as
the instruction is concerned: its write waits in the access-fault frame's
write-back registers, and the chip leaves the handler to do it
(MC68040 User's Manual 8.4.6). `kernel/wb040.c` completes them after
demand paging makes the page -- which is what a program's first write
to any new page needs -- and when a fault becomes a signal, carries the
writes it could not make in the signal frame for `sigreturn` to make
once the handler has made them possible. QEMU re-runs a faulting
instruction instead and never leaves a write-back, so none of this runs
under emulation; it is written from the manual and first runs on the
chip.

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

**A blocked signal can be TAKEN rather than delivered**:
`rt_sigtimedwait` (177, and 421 with a 64-bit timespec) returns the
lowest pending signal of a set and clears it, sleeping until one comes
or the timeout, and libc's `sigwait`, `sigwaitinfo` and `sigtimedwait`
are built on it. A blocked signal wakes no sleeper, so the wait is
poll's: raising one calls `poll_wake()`, as it does for signalfd.
`tgkill` in libc sends to one thread with the signal number translated
-- `syscall(__NR_tgkill, ...)` would hand the kernel picolibc's
number. `personality` (136) keeps and reports Linux's execution-domain
word; `ADDR_NO_RANDOMIZE` is true whatever it says, since nothing here
is randomized.

### Tracing: ptrace

`ptrace(2)` is Linux/m68k's, request for request and number for number
(`ptrace.c`), so a tracer built for Linux drives it unchanged -- which
is the point: `strace` is the stock 7.2 source and nothing in it knows
this is not Linux. The requests are TRACEME, ATTACH, DETACH, PEEK and
POKE of text, data and the user area, CONT, SYSCALL, SINGLESTEP, KILL,
GETREGS/SETREGS, GETFPREGS/SETFPREGS, SETOPTIONS, GETEVENTMSG and
GET/SETSIGINFO; the options are TRACESYSGOOD, the fork, vfork, clone,
exec and exit events, and EXITKILL.

A traced task stops in four places, each where Linux stops it: on the
way into and out of a system call (`syscall_dispatch`), when a signal is
about to be delivered (`signal_deliver`), at the end of `exec`, and as
it begins to exit. The tracer learns of a stop through `wait`, as a
stopped status -- `SIGTRAP|0x80` for a system call under TRACESYSGOOD,
the event number in bits 16-23 for the others -- and a tracee is
`t (tracing stop)` in `/proc/PID/stat`, with its tracer's pid in
`TracerPid`.

The register file is the one `PEEKUSER` describes: d1-d5, d6, d7,
a0-a6, d0, the user stack pointer, `orig_d0`, SR and PC, in that order.
A tracer that sets `orig_d0` to -1 at a system call's entry skips the
call, as Linux's does. Single-stepping sets the 68040's T1 bit for one
instruction. **Poking text** makes the page private first, as
copy-on-write would, and flushes the caches afterwards -- a breakpoint
written into a page the program shares with every other process
running it would otherwise be a breakpoint in all of them.

**strace is built against musl**, not picolibc (`ports/musl`). It
decodes by Linux's numbers -- signals, clocks, `AT_*` flags, the
`siginfo` layout -- and picolibc numbers several of those its own way,
so every one would need translating twice. musl is Linux's numbers all
the way down. It is static; `ports/musl/sage040-crt1.c` turns this
kernel's start-up stack into the contiguous argc/argv/envp/auxv block
musl expects. Two things it needed from the kernel: `readv`/`writev`
(musl's stdio writes nothing else), and Linux's `utsname` layout, six
fields of 65 bytes.

**The signal trampoline is always the kernel's.** Linux/m68k ignores
`sa_restorer` and musl relies on that: its restorer is a placeholder,
and a kernel that honoured it returned from every handler into nothing.
The trampoline is written to the user stack, so the caches are flushed
after it.

`kernel/ptracetest.sh` drives each request from a small tracer;
`kernel/stracetest.sh` checks strace's account of a known list of calls,
`-f`, `-c`, `-e` and `-p`.

**gdb runs on the machine** (`ports/gdb`, 17.1, native gdb and
gdbserver), over the same ptrace -- built as a Linux host, so that it
gets linux-nat. Four things it needed:

- **C++ exceptions**, which had never worked here: nothing registered a
  program's unwind tables (toolchain.md, `libc/crtbegin-eh.s`). gdb
  throws as ordinary control flow and died at its first.
- **The OS ABI.** Nothing built here carries an ELF ABI note, and gdb's
  m68k sniffer called such a file SVR4, whose breakpoints do not step
  the pc back over the trap: every breakpoint was missed and the
  program resumed mid-instruction. And this gcc returns a pointer in
  `%d0` and passes a structure's return address in `%a0`, where Linux
  says `%a0` and `%a1`. `ports/gdb/patches/02`.
- **`/proc/<pid>/mem`**, without which gdbserver cannot insert a
  breakpoint at all, and **`/proc/<pid>/task/<tid>/`**, where native
  gdb opens it. Native gdb falls back to PEEK and POKE when mem cannot
  be written -- but it decides that by writing `/proc/self/mem`, so once
  `mem` existed and `task/` did not, every breakpoint failed.
- **gdbserver's target description**: m68k's has none, and gdbserver
  asserted when asked for it (`patches/03`).

gdb starts a program with `$SHELL -c exec PROG`, and **the console's
`SHELL` is `/bin/msh`**, which is not a POSIX shell and has no `exec`:
`run` answers "exec: command not found". Under bash, or with
`SHELL=/bin/sh`, it works. `kernel/gdbtest.sh` checks a breakpoint,
`bt`, values, `list`, `finish` (an int and a pointer), `next`, a signal
stopped and passed on, the exit status, `kill`, attaching with `-p`,
and gdbserver driven over loopback.

---

## System calls

Forty of them. The convention and the numbers are Linux's; the calls
above 400 are local, because Linux has nothing to match.

| | |
|---|---|
| **Files** | `open` `close` `read` `write` `lseek` `ioctl` `unlink` `rename` `stat` `getdents` `fsync` `sync` `statfs` `mount` `umount2` |
| **Directories** | `mkdir` `rmdir` `chdir` `getcwd` |
| **Tasks** | `exit` `getpid` `kill` `waitpid` `sched_yield` `spawn` (1000) `jobctl` (1001) |
| **Time** | `time` `stime` `times` `nanosleep` |
| **System** | `uname` `sysinfo` `reboot` |
| **Network** | `socket` `bind` `connect` `listen` `accept` `sendto` `recvfrom` `shutdown` `netctl` (402) |

`fork`, `execve` and `waitpid` are Linux's. `fork` shares the address
space copy-on-write, so fork-then-exec copies almost nothing. `spawn` is
still here, fork and exec in one call: a path and an argument vector,
and a new task.
`/bin/sh` is **bash**, a link that ports/bash installs, and it is what
`sh -c`, `system()`, `popen()`, make's recipes and every `./configure`
run -- all of which assume a POSIX shell, which the system's own shell
is not. Run as `sh`, bash is in its POSIX mode. The kernel's own shell
built as a program is **`/bin/msh`**. A disk with the system and no ports
gets `/bin/sh -> msh`, so logins still find a shell; reinstalling the
programs never replaces bash's link. `kernel/shtest.sh`.

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

**`/dev` is listed from the registry.** The devices are names, not
inodes: `resolve_dev()` turns `/dev/<rest>` into the device of that name
(which is how `pts/0` is a device whose name has a slash in it). So
`/dev` and `/dev/pts` are directories of procfs's tree, listing the
registry as it stands, and `/dev/fd`, `/dev/stdin`, `/dev/stdout` and
`/dev/stderr` are links into `/proc/self/fd`, exactly as on Linux --
which is what bash's `<(...)` opens. Everything else under `/dev` is the
registry's, and `/dev/shm` is tmpfs. A name there that is no device is
`ENOENT`. There is no `/dev/tty` (the controlling terminal) yet.

**Every file says which filesystem it is on, and every device which
device it is.** `struct stat` carries `st_dev` and `st_rdev`: the disk
is 3:1, as Linux numbers the first IDE partition, and the rest are
Linux's anonymous majors -- `/proc` 0:4, `/dev` 0:5, sockets 0:8, pipes
0:12, eventfd and friends 0:13, tmpfs 0:21. Each device has an inode of
its own in `/dev` and Linux's number where Linux has the device (`null`
1,3, `console` 5,1, `ttyS0` 4,64, a pty 136,N), or one in the local
range, 240, where it has not (`kbd0`, `fbcon`). Before this every
device was inode 1 of the root disk with no number, so the console and
`/dev/null` were the same file -- GNU cmp, which skips writing to
`/dev/null`, printed nothing -- and `lstat` of a device failed. picolibc
packed the numbers into its 64-bit `dev_t` so that `major()` could not
unpack them (libc patch 46).

### /tmp and /dev/shm: tmpfs

`/tmp` and `/dev/shm` are **tmpfs** (`fs/tmpfs.c`): files in pages of
memory, so scratch files cost no disk writes and are gone at the next
boot rather than surviving a crash as litter. The disk's own `/tmp`
directory is still there, hidden under it. `shm_open(3)` makes its files
in `/dev/shm`, and they map `MAP_SHARED` like any file (the text cache's
pages, see "Shared pages").

It is a `struct fs_type` like ext2's, and `vfs.c` routes to it by path
(`vfs_route`): a path under `/tmp` or `/dev/shm`, or a relative one from
a working directory there, is tmpfs's; everything else is the disk's.
Every open file records which filesystem it came from. So permissions,
the text cache, shared mappings and descriptors work on its files
exactly as on the disk's. Regular files, directories, hard and symbolic
links, modes, owners and times; the sticky bit on `/tmp` is enforced.
Half the machine's memory at most; `ENOSPC` past it. A rename or link
between tmpfs and the disk is `EXDEV`, as between any two filesystems
(`mv` copies); a symbolic link in tmpfs that leads out of it is refused
with `EXDEV` rather than followed onto the disk. `statfs` of a path in
it reports tmpfs and its limits, so `df /tmp` shows what is left of them.

**`open` and `mkdir` honour their mode now**, less the umask, on every
filesystem. The umask starts at 022, as Linux starts init: task 0, which
every process inherits it from, used to be built with 0, so every file
anybody made was writable by everyone. They used to be ignored -- every file was created 0644 --
so a program making a private file with 0600 got one anybody could read.

### sendfile, splice, copy_file_range, mremap, memfd_create

`sendfile`, `splice` and `copy_file_range` (`xfer.c`) move bytes from
one descriptor to another with Linux's rules -- splice needs a pipe at
one end and no offset on it; copy_file_range takes two regular files
and refuses overlapping ranges of one; sendfile refuses an `O_APPEND`
output -- through one page of the kernel's memory, a read and a write
at a time. On Linux they avoid copies; here they save the program a
buffer and two system calls a chunk. What matters to a port is that
they exist, because a web server's sendfile or cp's copy_file_range has
no fallback, or one taken only on `ENOSYS`.

`mremap` moves page DESCRIPTORS, not bytes (`vm_move`): each page keeps
what it was -- lazy, swapped out, a file's shared page -- and nothing is
copied, which is what makes a growing realloc cheap. Pages added by
growing are new anonymous ones. With no table of mappings (see the head
of `mmap.c`), nothing remembers that a mapping came from a file, so growing a
FILE mapping adds zero pages where Linux would map more of the file;
shrinking and moving one are exact.

`memfd_create` is a tmpfs file made under `/dev/shm` and unlinked at
once -- all a memfd is on Linux, too -- so it reads, writes and maps
`MAP_SHARED` like any tmpfs file and goes with its last descriptor.
`/proc` names it `/memfd:NAME (deleted)`. No sealing: `F_ADD_SEALS` is
`EINVAL`.

### FIFOs

`mkfifo` makes a named pipe on the disk or in tmpfs: an inode with no
data (type FIFO in the ext2 directory entry, which e2fsck checks), and
opening it JOINS a pipe (`pipe.c`) -- the first open of that inode makes
the ring, every later one shares it, and it goes when both ends have
been closed everywhere. So two programs that share nothing but a path
can talk.

Opening waits for the other end, as POSIX says: a reader until a writer
has opened, a writer until a reader has. `O_NONBLOCK` lets a reader
through at once (reading end of file until a writer comes) and makes a
writer with no reader `ENXIO`; `O_RDWR` is both ends and never waits,
which is Linux's answer. `mknod` makes a FIFO or an empty file; a device
node is `EPERM` (see "What it is not").

### Record locks

`fcntl`'s byte-range locks are real (`reclock.c`): `F_GETLK`, `F_SETLK`,
`F_SETLKW`, their 64-bit forms, and Linux's open-file-description locks,
`F_OFD_*`. A POSIX lock belongs to the PROCESS -- two descriptors in one
process never conflict, and closing ANY descriptor for the file drops
every lock the process has on it, POSIX's famous wart, which SQLite is
written around. An OFD lock belongs to the open file, as flock()'s
does: a dup shares it, a second `open` is a different owner even in the
same process, and it lasts until the last close. Taking a lock over a
range already held replaces what was there -- splitting and merging --
which is how a read lock is upgraded and part of one released.
`F_SETLKW` that would close a circle of processes waiting on each other
is `EDEADLK`. Locks go when their process exits.

They used to be faked in libc (`patches/32`): every lock "succeeded" and
`F_GETLK` always said nothing was in the way, which was true only
because nothing could lock anything. `patches/44` sends them to the
kernel, translating picolibc's `struct flock` (its `l_type` values are
one higher than Linux's, its `l_pid` a short).

### Waiting on everything at once: epoll, eventfd, timerfd, signalfd, inotify

Linux's event descriptors, with Linux's numbers, flags and record
layouts (`events.c`), so an event loop -- libuv, libevent, GLib's main
loop, nginx -- has what it looks for:

| | |
|---|---|
| `eventfd` | a 64-bit counter: `write` adds, `read` takes all of it (or one, with `EFD_SEMAPHORE`) |
| `timerfd` | a timer on `CLOCK_MONOTONIC` or `CLOCK_REALTIME` (`TFD_TIMER_ABSTIME` too); a read returns how many times it has fired |
| `signalfd` | reads the reader's pending signals of those in its mask, 128 bytes each, and takes them |
| `epoll` | a set of descriptors: level-triggered, `EPOLLET`, `EPOLLONESHOT`, nesting, `epoll_pwait` |
| `inotify` | watches on files and directories: create, delete, modify, attrib, open, close, access, rename (with a cookie), delete-self, move-self, overflow |

**POSIX timers** -- `timer_create`, `timer_settime`, `timer_gettime`,
`timer_getoverrun`, `timer_delete` -- are the same timers delivering a
signal instead of a readable descriptor: `SIGEV_SIGNAL` or `SIGEV_NONE`,
on either clock, absolute or relative, with an overrun count. Thirty-two
at once, machine-wide; a process's go with it at exec and exit.

`ppoll` and `pselect` come with them, because `epoll_pwait` needed the
same thing: a signal mask for the length of a wait, which lets a signal
in only while waiting.

**One wait is poll's wait.** Whatever makes one of these ready calls
`poll_wake()`, and every blocking read, `poll`, `select` and
`epoll_wait` sleeps on poll's queue -- so nothing has a queue of its
own, and an epoll holding a pipe, a socket, an eventfd, a timerfd and a
signalfd is one wait that any of them ends. A timer is the exception:
nothing happens when it expires, so it says when it will
(`poll_deadline`) and the sleep is cut to that. **Timers are lazy**:
nothing counts expirations as they pass; whoever looks works out how
many there have been. At the tick's resolution, 10 ms, like `setitimer`.

epoll asks rather than being told: `epoll_wait` scans its set with the
same poll routine `poll()` uses, starting each scan after the last
descriptor it reported, so one that is always ready cannot hide the rest
from a small `maxevents`. `EPOLLET` reports a descriptor when it GAINS a
bit since it was last looked at. The set does not hold its files open --
closing the last descriptor for a file takes it out of every epoll, as
on Linux. A regular file or a directory is `EPERM`, a cycle of epolls
`ELOOP`. `struct epoll_event` is 12 bytes, because a 64-bit member needs
only 2-byte alignment on m68k.

inotify watches INODES. `vfs.c` wraps every call that changes something
and reports it once it has succeeded; nothing below that knows. With no
watch anywhere each hook costs one test of a word. Each inotify has one
page of queue; a full one ends in a single `IN_Q_OVERFLOW`, and an event
identical to the last unread one is merged into it. It works the same on
the disk and in tmpfs.

**picolibc numbers signals its own way** -- `SIGUSR1` is 30, Linux's is
10 -- so libc translates every mask it passes, and `read()` translates
the signal numbers in what it reads from a descriptor `signalfd()`
made (`libc/patches/43`). A signalfd reached by `dup` or inherited
across `exec` reads Linux's numbers.

### /proc

Linux's `/proc`, the part ported software reads (`procfs.c`), made out
of the kernel's own tables the moment a file is opened -- a snapshot,
so a reader taking it in small pieces sees one moment -- and read-only,
except `/proc/<pid>/mem`.

| | |
|---|---|
| `/proc/cpuinfo` | Linux/m68k's layout: CPU, MMU and FPU 68040. No clock figures, because none is measured |
| `/proc/meminfo` | MemTotal, MemFree, MemAvailable, Cached, SwapTotal, SwapFree: Linux's exact line shape, which `free` splits on |
| `/proc/loadavg` `/proc/uptime` | the load average, running/total tasks and last pid; uptime and idle time |
| `/proc/stat` | `cpu` and `cpu0` (user, system and idle ticks), `btime`, `processes`, `procs_running` |
| `/proc/mounts` | the root volume, `/dev` and `/proc` |
| `/proc/self` | a link to the caller's own directory |
| `/proc/<pid>/` | `cmdline` `environ` `comm` `stat` `statm` `status` `maps`, and the links `exe` `cwd` `root` `fd/<N>` |
| `/proc/<pid>/task/<tid>/` | one directory per thread of the process, with the same files about that thread (and no `task/` of its own): how gdb lists a process's threads, and where it opens `mem` |
| `/proc/<pid>/mem` | the process's memory, the file offset being the address, read AND written: what a debugger uses (gdbserver uses nothing else). The process itself, its tracer, or its own user and root; a write to text makes that page private first, as a POKE does |

The formats are Linux's field for field -- `stat` is all fifty-two --
because the only reason to have `/proc` is that programs written for
Linux parse it. A figure this kernel does not keep (a process's
controlling terminal, fault counts) is a 0 in its place, not a missing
field.

**The links are followed like any symbolic link**, anywhere in a path:
`/proc/self/cwd/notes.txt` is a file on the volume, and opening
`/proc/<pid>/fd/3` opens what descriptor 3 is -- afresh, for a path; the
same open pipe, for a pipe. `fd/<N>` names what the descriptor was opened
by (a path made absolute, or `pipe:[N]`, `socket:[N]`); a rename after
the open is not followed, where Linux's is.

**A process's links, its `fd/` and its `environ` are its own user's and
root's**; everything else is readable by all. Nothing in `/proc` but
`mem` can be written, and nothing created, removed or renamed.

**The working directory can be in `/proc`**, as sbase's `ls`, `find` and
`du` need -- they change into every directory they list. It is kept as a
path alone, and while it is under `/proc` every relative name is
`/proc`'s. `..` is taken by name there: `/proc/self/cwd/..` is
`/proc/self`.

**`maps` is read off the page tables**, since there is no table of
mappings (see `mmap.c`), and named from what exec recorded: the program,
its interpreter, `[heap]` and `[stack]`. A shared library's pages are
unnamed, because nothing records which file a text-cache page came from.
`x` comes from the ELF segment's flags, the 68040 having no execute bit.

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

#### The journal

The volume is made with a journal (`mke2fs -j`), ext3's: ext2 plus a log
in inode 8, in the JBD format Linux and e2fsprogs read. `fsimg.sh journal`
adds one to an existing volume in place; `FS_JOURNAL=0` makes one without.

**What it buys:** a machine stopped at any instant -- power cut, QEMU
killed, a panic -- leaves a volume that is exactly what some set of calls
made, never something between two of them, and the next mount makes it so
without a check. Before the journal the answer was the boot-time check,
which repairs what it can recognise; a crash mid-update is the case a
check cannot always get right.

**How:** metadata a call dirties -- bitmaps, inodes, group descriptors,
indirect and directory blocks, the superblock -- stays in the block cache
(pinned, never evicted) as the running transaction. A commit writes file
data home first (ordered mode: nothing committed can point at data older
than it), then a descriptor block and a copy of every pinned block into
the log, the journal superblock saying where, and a commit block; then
writes the copies home and marks the log empty. A stop before the commit
block leaves the old volume; after it, replay rewrites the copies over
whatever half-done checkpoint it finds.

**When:** only between calls (vfs.c tells the filesystem as its lock is
let go). At the end of every call that changes the namespace or an inode
-- close, unlink, rename, mkdir, link, symlink, chmod, chown, utime -- so
those are on the disk when they return, as they always were; once a
transaction is five seconds old (writes to a file still open), from
`kjournald` once a second when the machine is otherwise idle; when half
the cache is pinned; at `sync`, `fsync` and unmount; and, failing those,
when one call pins the whole cache. A block freed in the running transaction is not
given out again until it commits -- file data goes home before the
commit, and a crash would otherwise bring back the old file pointing at
the new one's bytes.

**Replay** happens at mount, honours revoke records (so a journal Linux
wrote replays correctly), and is also done by the host's `e2fsck` and by
Linux mounting the volume -- three implementations that share no code,
and `kernel/journaltest.sh` checks all three against a machine stopped
at a chosen point of a chosen commit (`kstat KSTAT_JOURNAL_STOP`).

**A damaged journal:** a journal superblock that cannot be read is made
afresh in place and, if it held anything, the volume is checked as after
any unclean stop. A journal of a kind this driver cannot replay (64-bit
block numbers, checksums -- Linux's ext4 defaults) holding something is
not refused: the volume is mounted without it and checked, because
refusing the root volume would leave a machine that cannot start.

**What it does not do:** journal file DATA (ordered mode, as ext3's
default), or checksum the log -- like ext3 without `journal_checksum`, a
copy corrupted inside a committed transaction would be replayed as it is.

**Linux reads and writes the same volume.** `kernel/linuxfstest.sh`
hands one volume back and forth with the Linux kernel's own ext3 driver
(a loop mount, so it needs sudo): a tree with every size boundary, every
permission bit, four owners, hard links, fast, slow and dangling
symlinks, UTF-8 and 255-byte names, a FIFO and set times, listed by both
sides down to the inode number and a hash of every byte; changed by
sbase's tools on the machine and by coreutils on Linux; and each side's
listing must equal the other's.

### More than one volume: mount

`mount /dev/hda2 /mnt` puts a second ext2 volume into the tree, and
`umount /mnt` takes it out; `mount` alone lists what is mounted
(`/proc/mounts`), `df` gives a line for each, and `mount -o ro` (or `-r`)
mounts one read-only. The disk's partitions are block devices of their own,
`hda1` to `hda4`, found in the MBR at boot (the banner counts them); a
partition is a window onto the disk, so a volume mounted from one needs to
know nothing about the table. Only root mounts, as on Linux, and only on a
directory of the disk: `/tmp`, `/proc` and `/dev` are routed by name before
the disk sees a path, so a volume mounted there could never be reached.

**The mount table is ext2's, not the VFS's.** Crossing from one volume to
another happens in the middle of walking a path -- `/mnt/d/../../etc` goes in
and comes out again -- and the walk is the filesystem's. So `fs/ext2.c` keeps
every mounted volume in a `struct ext2_vol` (its device and geometry, its
block cache, its journal, its open inodes), and the lookup a walk makes
(`lookup_x`) turns a name that is a mount point into the mounted volume's
root, and `..` at a mounted root into the parent of the directory it covers.
Lookups that are about to change an entry -- unlink, rmdir, rename -- use the
plain one and say `EBUSY` for a mount point.

**Outside ext2.c an inode is named by a handle**: the volume's index in the
top byte, the inode number below. The root volume's index is 0, so its
handles are plain inode numbers and everything that held one before -- a
working directory, a chroot, an open directory -- holds the right thing.
`stat` gives the real inode number and a device of the partition's own
(`3:2` for hda2, as Linux numbers it); what keys on a file internally (the
text cache, the lock table, inotify, FIFOs, swap) uses `vfs_file_key()`,
the two together, because each volume numbers its inodes from 1.

A hard link or a rename across volumes is `EXDEV`, which is what makes `mv`
copy instead. A volume cannot be unmounted while anything uses it -- a file
open on it (which includes a program running from it), any task's working
directory or root on it, or another volume mounted on it -- and the answer is
`EBUSY`; there is no lazy unmount. `halt`, `shutdown` and `reboot` unmount
every volume, the most recent first, so each is left clean. A read-only
mount replays its journal if the volume needs it (Linux does too) and then
writes nothing at all, not even the mark that says it is in use.

`statfs(2)` and `fstatfs(2)` are Linux's now -- a path or a descriptor --
where `statfs` used to take only the buffer and answer for the root; with
`statfs64` and `fstatfs64`, which picolibc's `statvfs` and `fstatvfs` call
and which used to be missing. `kernel/mounttest.sh` checks
all of this on a disk of three partitions, judging what is left with the
host's tools: every volume clean by e2fsck after the halt, the files on the
right volume, the read-only one bit for bit what it was, and a sync on hda2
stopped after its commit block replayed by the next mount.

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

## The terminals

`tty.c` is a line discipline, not a driver, and there are TWO independent
terminals built from it, each `struct tty` with its own input ring,
termios, foreground group and size:

  - **`/dev/tty1`** -- the screen: keyboard in (`kbd0`), framebuffer out
    (`fbcon`). 80x30, which is what the framebuffer measures.
  - **`/dev/console`** -- the serial line: `ns16550.c`'s UART in and out.
    80x24 until `stty`/`resize` says otherwise. Kernel messages go here.

They are NOT mirrored: what is written to one does not appear on the
other, and each reports its own size, so a full-screen program on the
screen gets 30 rows and one on the serial line gets 24 -- neither clamps
the other. A getty runs on each, so you log in wherever you are sitting.
This replaced a single console that fanned output to both and merged
their input, which forced a program to fit the smaller of the two and so
made the 80x30 screen behave as 80x24.

Echo goes to a terminal's own output, never back to its input. Otherwise
what you type on the screen would appear only on the wire the character
came in on.

**A terminal's output is not exclusive and must not become one.** The
test harnesses drive the SERIAL terminal with `-display none`, and QEMU
delivers no keyboard input without a display; the screen terminal sits at
its getty meanwhile. Nothing depends on the two being the same terminal.

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

## Sessions and controlling terminals

A **session** is a login: `setsid()` starts one, and its leader is the
first program of it. A session has at most one **controlling terminal**
-- the screen, the serial line or a pty -- and a terminal controls at
most one session (`kernel/ctty.c`). The rules are Linux's:

- a session leader with no controlling terminal that opens a terminal
  nobody controls gets it, unless it says `O_NOCTTY`;
- `TIOCSCTTY` asks for it explicitly, and root (argument 1) may take one
  another session holds; `TIOCNOTTY` gives it up; `TIOCGSID`
  (`tcgetsid`) says which session a terminal controls;
- a job may be put in front (`TIOCSPGRP`) only by its own session, on
  its own controlling terminal;
- `/dev/tty` is the opener's controlling terminal, ENXIO without one,
  and `ctermid()` names it;
- **a hangup** -- a pty's master closing, as when an ssh connection
  drops -- sends SIGHUP and SIGCONT to the session's leader and its
  foreground job, and the session loses the terminal; **the leader
  exiting** does the same to the foreground job;
- `/proc/<pid>/stat`'s `tty_nr` is the controlling terminal's device
  number, so `ps` can say which terminal a process is on.

A **console login** is a session: `/bin/login`, started by the system's
shell inside its own session, forks, and the child starts the session
and takes the terminal (as a getty-started login does on Linux). An ssh
login is one too -- Dropbear's own `setsid` and `TIOCSCTTY` now work, and
its log no longer says "Failed to disconnect from controlling tty".
The console's own shell is a kernel task and outside all of this.

**Dropbear closes a pty channel's master when the client sends EOF**, and
a client whose standard input is at end of file sends it at once -- so
`ssh -t host cmd < /dev/null` hangs `cmd` up. That is Dropbear's
behaviour and Linux's would be the same; a person's terminal never sends
it. It was long noted here as "ssh -t output never arrives".
`kernel/sesstest.sh` checks every rule above from processes built for
it; `kernel/sshtest.sh` the ssh end.

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
  permissive that applies": a file mode `0477` denies its owner the
  write that everybody else is given, because the owner matched first
  and the search stopped there. That is the POSIX rule rather than an
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

**A file that begins `#!` is run by the program it names**, as on Linux
(binfmt_script): `#!/usr/bin/perl -w` runs `/usr/bin/perl -w SCRIPT
ARGS...`. The rest of the line after the interpreter is ONE argument,
spaces and all; the line is at most 256 bytes; a script may name a
script, four deep, then ELOOP. The script needs execute permission and
so does its interpreter, and set-user-id comes from the interpreter,
never from a script. A file starting `#` without the `!` still fails
with ENOEXEC, and the shells run it themselves. `kernel/shebangtest.sh`.

`lib/` is what a program written for this system links against: `crt0.s`,
`ulib.c`, `user.ld`. `system/` is what the system ships, installed into
`/bin`: `ifconfig`, `ping`, `netstat`, `host`, `ntpdate`, `shutdown`,
`env`, `stty`, `resize`, `fsck`, `df`, `mount`, `umount`, `id`, `klogd`, `dmesg`,
`swapon`, `swapoff`, `nvram`, `irqs` and `msh` (the system shell; `/bin/sh`
is bash). `apps/` is everything
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

`ld.so` also lays out **thread-local storage** -- the program's and every
start-time library's, in one block per thread, in Linux/m68k's layout --
and stays in the process afterwards, so that **`dlopen`** can load a
library, its dependencies and its TLS while the program runs. The C
library reaches it through a table `ld.so` hands it at start
(`sage040-dl.h`). Every library is global and none is ever unloaded, as
in musl; `libc/README.md` has the rest.

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

`ls` reads a directory the way a C library's `readdir` does, by opening
it and calling `getdents64`, so it lists `/proc` like anything else;
`ls -l` asks `lstat` about each name and shows a link's target.

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
bare-metal device tests, the crypto vectors and the disk staging on the
host, and thirty-five scripted suites that each boot the machine and
drive it over its serial line.

| | | |
|---|---|---|
| `tests/` | 12 programs | the devices: CPU and FPU, UART, ATA, MFP and its timers, MMU, SM501, RTC, keyboard |
| `kernel/cryptotest.sh` | 4 | ChaCha20 and BLAKE2s against the RFCs, built for the host |
| `kernel/fstest.sh` | 69 | the filesystem, names of any shape included, and a file past what one indirect block reaches -- verified with the host's debugfs and e2fsck |
| `kernel/apitest.sh` | 376 | the system call surface a ported program expects |
| `kernel/faulttest.sh` | 15 | a program's faults as signals it catches and survives: SIGSEGV repaired by mprotect and retried, SEGV_ACCERR and si_addr, SIGILL stepped over, SIGFPE, SIGTRAP, a blocked fault still fatal; writing its own code or a string literal is SIGSEGV |
| `kernel/procfstest.sh` | 73 | `/proc` against what a program knows for itself -- getpid, argv, environ, its own inode, sysinfo, where `main` and a local are -- and read by sbase's `cat`, `ls` and `readlink`; refusals, another user's process, stopped and zombie states, standing in `/proc` |
| `kernel/fpsptest.sh` | 17 | the 68040's missing FPU instructions: 59 results from Motorola's FPSP (`fpsp-trap=on`) and from QEMU, each against the host's libm; two at once; F-line as SIGILL; enabled divide by zero, operand error, signalling NaN and BSUN each a SIGFPE with its si_code; FMOVEM's control-register order |
| `kernel/edittest.sh` | 41 | the line editor, history, job control, command lists, scripts, shutdown |
| `kernel/vmtest.sh` | 18 | what a program cannot touch |
| `kernel/pagetest.sh` | 51 | demand paging, copy-on-write, swap, and running out of memory |
| `kernel/nettest.sh` | 19 | ARP, DHCP, ICMP and TCP against the host; the SYN's options decoded there |
| `kernel/lotest.sh` | 22 | the loopback interface, and 127/8 frames forged onto the wire |
| `kernel/dnstest.sh` | 57 | both resolvers and `ntpdate`, against servers on the host |
| `kernel/tcptest.sh` | 24 | TCP's options, loss, keepalives and TIME_WAIT |
| `kernel/vttest.sh` | 95 | the VT102 console, checked against screenshots |
| `kernel/devtest.sh` | 41 | interrupts, the filesystem under concurrency, the limits, the NVRAM, `mmap` of the framebuffer |
| `kernel/libctest.sh` | 269 | picolibc and the POSIX layer added to it -- `sigsuspend` really sleeping, `sigwait`, `sigtimedwait`'s timeout, `sigwaitinfo` woken by another process, `personality`, `tgkill` among the latest; and sigcowtest, a caught SIGCHLD arriving during copy-on-write faults |
| `kernel/sotest.sh` | 126 | shared libraries, `ld.so`, and the sharing of their pages |
| `kernel/tmpfstest.sh` | 36 | tmpfs at `/tmp` and `/dev/shm`: files, holes, truncate, links, rename and `EXDEV`, the working directory, `shm_open` shared between processes, the sticky bit, and from the host: the disk's `/tmp` hidden, nothing written to it |
| `kernel/eventtest.sh` | 113 | eventfd, timerfd, signalfd, epoll and inotify, ppoll/pselect, and POSIX timers (`timer_create` and the rest): counts, blocking and waking by another process, timers timed by CLOCK_MONOTONIC, signals checked gone from `sigpending`, level/edge/oneshot, one epoll over all four kinds, masks that let a signal in only during the wait, inotify on the disk and in tmpfs, queue overflow |
| `kernel/locktest.sh` | 54 | fcntl record locks, POSIX and OFD, on the disk and in tmpfs, seen from a second process: conflicts, F_GETLK naming the holder, splitting, read locks shared, the close-drops-all wart, F_SETLKW waiting, EDEADLK, release at exit, SEEK_END and negative lengths; F_DUPFD's argument |
| `kernel/fifotest.sh` | 38 | named pipes on the disk and in tmpfs: two processes through a path, an open that waits for the other end (timed), end of file, O_NONBLOCK and ENXIO, O_RDWR, EINTR, unlink while open; and from the host, e2fsck clean and debugfs seeing the FIFO |
| `kernel/devdirtest.sh` | 26 | `/dev` as a directory: every listed device stats as one, a new pty appears in `/dev/pts` under its ptsname, `/dev/fd` and `/dev/std*` are the links Linux has, `/dev/fd/N` of a pipe shares the pipe and of a file reopens it, a working directory in `/dev`; every device an inode of its own and Linux's number, the terminal not the same file as `/dev/null`, `lstat` of a device, five filesystems told apart by `st_dev`; `cd /dev` at the console, and `/bin/ls -l` there |
| `kernel/xfertest.sh` | 39 | sendfile (file to file, with an offset, into a pipe five times its size), splice both ways and its refusals, copy_file_range, mremap (moved with its contents, the old address faulting in a second process, shrunk, grown in place, a moved MAP_SHARED mapping still the file), memfd_create; and from the host, the files moved compared byte for byte |
| `kernel/linetest.sh` | 43 | lines drawn by the SM501's own Line Draw, judged from a screendump by `lineprobe.py`: every octant from both ends the same pixels, end points, one pixel per major step, within half a pixel of the true line, clipping at three edges exactly the on-screen part, nothing stray; a line past the engine's range refused |
| `kernel/gfxtest.sh` | 66 | the graphics demos: each started, screendumped twice a second apart, stopped with q -- exits 0, draws a picture, moves, prints its summary; sorts leaves every array sorted; mandel's fixed-point and FPU pictures agree on the set pixel for pixel, and the centre of its view is in it |
| `kernel/shmaptest.sh` | 21 | `MAP_SHARED` of a file against read() and write(), a forked child, another process, a second mapping and `/proc/self/maps`; msync, munmap and exit writing back; truncate; a mapping outliving its name; and a file written only through a mapping, checked byte for byte from the host |
| `kernel/tlstest.sh` | 46 | `__thread`, static and dynamic: initial values, a fresh copy per thread, alignment, fork; a start-time library's TLS reached two ways that must agree; `dlopen` of a library with TLS, a dependency and a constructor, its TLS made per thread on first use; refusal and rollback of an initial-exec TLS library; `dlsym` scopes, `dladdr`, `dl_iterate_phdr`, `dlerror` |
| `kernel/fscktest.sh` | 31 | `fsck`, against ten kinds of damage made on the host -- including a torn directory block, a name for a freed inode and a live block marked free -- each repaired in one pass and then agreed with by e2fsck; the clean flag on a volume without a journal, and no check needed after a crash with one |
| `kernel/journaltest.sh` | 30 | the journal: the machine stopped (a kernel knob) after a commit block and before one, each replayed by the host's e2fsck, by Linux mounting it and by this kernel; killed five times mid-storm, never needing more than the journal; the journal superblock and a commit block destroyed; and the control, the same storm with no journal, leaving damage e2fsck finds |
| `kernel/linuxfstest.sh` | 31 | the volume handed back and forth with Linux's own ext3 driver (needs sudo): a tree of every size boundary, permission bit, owner (past 65535 too), link kind and name shape, listed by both down to inode, time and a hash of every byte; changed by sbase's tools here and coreutils there, each side's listing equal to the other's; setgid inheritance; and the same 3000-call storm on both, three seeds, every call's result and the final trees identical |
| `kernel/fattest.sh` | 10 | the FAT16 fallback, which is no longer the machine's own filesystem |
| `tools/fsimgtest.sh` | 40 | the host's end of the disk, which every other suite stages its files through |
| `kernel/uemacstest.sh` `kernel/vitest.sh` | 9, 9 | the two editors |
| `kernel/awktest.sh` | 40 | awk's own regression tests, and eleven more against the host's awk |
| `kernel/sedtest.sh` | 21 | sed, against the same sed built for the host |
| `kernel/greptest.sh` | 37 | grep's own 329 pattern cases, and its options against the host's grep |
| `kernel/sbasetest.sh` | 77 | the utilities, against the host's own, and what only the disk can say |
| `kernel/bashtest.sh` | 13 + 4 known | the shell language against the host's bash, and part of bash's own suite. Four of bash's tests are expected to fail and are reported `[KNOWN]` with the reason -- `func` and `glob` want `/dev/fd` and a locale, which this system has not got; `type` and `varenv` are not diagnosed yet. One that starts PASSING is a loud failure |
| `kernel/threadtest.sh` | 52 | threads: clone, futexes, and the pthread layer, with the lock's own negative control |
| `kernel/ptytest.sh` | 34 | pseudo-terminals, and that the pairs are given back |
| `kernel/curstest.sh` | 28 | terminfo and curses, with the database renamed away as the control |
| `kernel/lesstest.sh` | 12 | less, and a full-screen program on a terminal that cannot address its cursor |
| `kernel/logtest.sh` | 10 | the kernel's log, klogd, and /var/log/syslog |
| `kernel/crontest.sh` | 8 | something the machine does by itself, later |
| `kernel/pytest.sh` | 43 | CPython, against the host's Python's answers to the same questions |
| `kernel/perltest.sh` | 37 | Perl: 64-bit integers, byte order, the XS modules against the host's digests and zlib, a `#!` script, perldoc, and an XS module built on the machine with CBuilder and with MakeMaker and GNU make |
| `kernel/gittest.sh` | 16 | git: a repository made on the machine passes the host's `git fsck --full --strict` with the history and author it was given, one made on the host is read, checked out and fscked on the machine; diff, a three-way merge, gc into a pack, `git submodule` (a `#!/bin/sh` script), a local clone through upload-pack, and a clone over HTTP through git-remote-http |
| `kernel/difftest.sh` | 16 | GNU diff and patch: the machine's diff applied by the host's patch and the host's by the machine's, exit statuses, `diff -r`, binary files, `diff3 -m`, `cmp`, `sdiff`, `patch -R`, `--dry-run`, `-p1`, and a hunk found at an offset |
| `kernel/ptracetest.sh` | 42 | ptrace from a small tracer: TRACEME and the exec stop, syscall stops with TRACESYSGOOD and their numbers and results, PEEK/POKE of data and of read-only text (the tracer's copy untouched), `/proc/<pid>/mem` and `task/<tid>/` read and written, a breakpoint hit with TRAP_BRKPT, SINGLESTEP, signal stops suppressed or delivered, GETFPREGS, ATTACH/DETACH and `/proc`'s view of a tracee, TRACEFORK, KILL |
| `kernel/stracetest.sh` | 15 | strace (built against musl): its account of a known list of calls -- strings out of the tracee's memory, errors decoded, a pid that has to be the tracee's own, the exit as `= ?` -- `-f` through a fork, `-c`, `-e trace=`, and `-p` attaching to a running process that runs on after |
| `kernel/gdbtest.sh` | 23 | C++ exceptions, static and dynamic; then native gdb: a breakpoint and the pc stepped back over it, `bt` through three frames, values, a string, a struct, `list`, `finish` returning an int and a pointer, `next`, `tbreak`, a signal stopped and passed on, the exit status, `kill`, `-p` attach and detach, and gdbserver driven over loopback |
| `kernel/shtest.sh` | 8 | `/bin/sh` is bash and the system shell `/bin/msh`, as the real install rules lay them out, read on the host; a `#!/bin/sh` continuation line and `system()` on the machine |
| `kernel/shebangtest.sh` | 20 | `#!`: the argv the interpreter gets, nesting and ELOOP, permissions, a set-user-id script ignored against an ELF control, from spawn and from execve |
| `kernel/usertest.sh` | 16 | uids and gids, `/etc/passwd` and `/etc/group`, and what an ordinary user is refused |
| `kernel/logintest.sh` | 14 | logging in: the right password gets that user's shell in that user's home and the wrong one does not, `su`, `sudo`, and the modes on them; and after a session dies without handing the terminal back, the next login still gets it |
| `kernel/linktest.sh` | 30 | hard links and symlinks -- one inode with two names, fast targets and slow ones, loops, dangling targets -- agreed with by the host's e2fsck; fast symlinks the host made surviving the boot-time check, and deleted without freeing their "blocks" |
| `kernel/dftest.sh` | 15 | `df` and `du` against the host's own figures for the same volume, with the shell's built-in as the control |
| `kernel/sesstest.sh` | 14 | sessions and controlling terminals: a new session has none, a leader opening a pty gets it (tcgetsid, the front, `/dev/tty`, `tty_nr`), a job inherits it and another session cannot use it, `O_NOCTTY`, `TIOCSCTTY` refused to a user and taken by root, the master closing hanging up leader and job, the leader exiting hanging up its job, `TIOCNOTTY`, `setsid` |
| `kernel/bootjtest.sh` | 7 | the boot ROM on a volume whose journal still needs replaying: a KERNEL.ROM renamed into place and the machine stopped after the commit, the host's home-blocks view still naming the old inode, the ROM loading the new one through the log, and the kernel replaying it clean |
| `kernel/mounttest.sh` | 98 | more than one volume: mount and umount, refusals (no such device, unknown type, a file, `/tmp`, `/`, the root's own partition by either name, not root), crossing into a volume and out by `..`, its own `st_dev`, `statvfs` and fsid, `EXDEV` both ways, `EBUSY` for an open file, a working directory and another process in it, and a volume mounted on it, a read-only volume refusing every kind of write; from the host, all three volumes clean after `halt`, the read-only one bit for bit unchanged, Linux reading what was written, and a journal on a volume that is not the root replayed by `mount` |
| `kernel/sshtest.sh` | 14 | ssh, scp and rsync against the workstation's own OpenSSH, which knows nothing about this project -- so the protocol is either right or it is not; `ssh -t` on a pty; a dropped connection hanging up the command; no TIOCSCTTY complaints from the server |
| `kernel/pylibtest.sh` | 27 | the libraries CPython is built against, proven by the programs that ship with them, every stream crossing the host boundary both ways |
| `libc/test/qemutest.sh` | 8 | a program built for this machine, run as a Linux/m68k binary by qemu-m68k user mode -- the system call numbers and errnos are Linux's, so something else can check them |

`make bashsuite` runs every one of bash's 83 tests instead of the subset,
which takes hours: one test is minutes of work for a 25 MHz 68040.

The picolibc suites need `make libc` first.

Everything they write goes in **`/tmp/scratch`**, and `make clean` removes
the lot -- outside the tree on purpose, because this one lives in Dropbox
and four 16 MB disk images rewritten by every run is a great deal of
syncing for files rebuilt from nothing each time. `SAGE_SCRATCH` moves it.
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

**No device nodes on disk.** `mknod` of a character or block device
answers `EPERM`: the devices are the names the kernel makes under
`/dev`, not inodes, so a node on the disk would name nothing. FIFOs,
hard links and symlinks, which used to be in this paragraph, work.

**A built-in can be shadowed by a program on purpose.** `df` is the
case: the built-in takes no arguments and prints one fixed report,
which is wrong for a command with `-h`, `-k` and `-i`. It runs
`/bin/df` when there is one and answers itself only when there is not
-- which is what keeps it useful on a disk whose filesystem is the
thing being investigated.

**No library is ever unloaded.** `dlclose` counts and returns; the
library stays, as in musl. And a library that reaches its TLS
initial-exec cannot be `dlopen`ed after start, only linked.

**One filesystem, one partition, one network interface.** The static
limits that were constants are larger now -- 64 tasks, 64 descriptors
each, 256 open files -- but these three are structure.

**Swap is a file, one at a time**, and there is no swap cache: a page read
back in gives up its slot, so evicting it again writes it again. When
memory is overcommitted and runs out, whoever faults is killed -- there is
no chosen victim.

A swap cache -- keeping the slot, and using the MMU's `M` bit to DROP a
page nothing had written rather than write it again -- was built and
taken back out. The short version of what went wrong, for anyone trying
it a second time, is that `M` lives on a descriptor while a remembered
slot belongs to a frame, and that the
kernel writes user pages by physical address, so `read(2)` filling a
buffer sets no `M` bit at all.

**No floating point in the kernel** -- of its own. The FPSP computes
on the FPU in supervisor mode, but on behalf of the program whose
instruction trapped, in that program's registers (see "Floating
point").

`design.md` keeps the list of what is planned, and what each would take.
