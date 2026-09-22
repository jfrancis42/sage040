# The Sage040 operating system

`README.md` describes the machine — a 68040, its chips, and the QEMU model
that provides them. This document describes the software that runs on it:
what it is, how it is put together, and why each piece is the way it is.

It is a real operating system in the sense that matters. It has protected
address spaces, preemptive multitasking, signals, job control, a
filesystem, a TCP/IP stack and a shell, and a program it runs cannot bring
it down. It is not a Unix clone, it is not POSIX, and it will not build
anything off the shelf. `emacs.md` works through exactly how far short it
falls, using GNU Emacs as the measuring stick.

There is no name for it beyond the machine's. It boots, it runs programs,
and that is the whole claim.

---

## Contents

- [Shape of the thing](#shape-of-the-thing)
- [Booting](#booting)
- [Memory](#memory)
- [Tasks](#tasks)
- [Waiting](#waiting)
- [Signals and job control](#signals-and-job-control)
- [System calls](#system-calls)
- [Files](#files)
- [The terminal](#the-terminal)
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
IDE disk, reads the partition table, loads `/KERNEL.ROM` from the FAT16
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
nothing needs contiguous physical memory. The machine's default is 4 MB and
the model accepts up to 2 GB.

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

There is no demand paging. An access fault kills the program — it has to,
because the 68040 pushes the address of the *faulting instruction*, so
`rte` re-runs it and faults again forever. And the SSW says nothing about
*why*: no bit distinguishes "not mapped" from "write protected" from
"supervisor only". A handler that needs to know must walk the tables or use
`ptest`.

---

## Tasks

A task is a kernel stack and an address space. `struct task` holds a pid, a
state, the saved kernel stack pointer, the address space, eight file
descriptors, signal masks and the job-control bookkeeping. There are eight
of them, statically allocated.

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

### Scheduling

Round robin over the ready tasks. No priorities; there is nothing here for
priorities to decide between. The idle task is *excluded from the scan*
rather than merely ranked below everything, because an idle task competing
on equal terms takes every other turn from whatever is actually working.

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

`fork`, `execve` and `waitpid` are Linux's. `fork` copies the address
space eagerly, since there is no copy-on-write yet. `spawn` is still
here, fork and exec in one call: a path and an argument vector, and a
new task, with no address space copied only to be thrown away.
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

## Files

`vfs.c` holds mounts, path resolution and open files. A descriptor is a
small integer indexing a per-task table of eight; the table entries point
at refcounted open-file objects, so two tasks can share one offset and a
dup would be free if there were a `dup`.

Descriptors are inherited by both `task_create()` and `exec`. That they are
inherited by *both* is a bug's worth of experience: a shell created without
them ran perfectly and silently, because it had no stdout, and that reads
as a broken context switch for a surprisingly long time.

### FAT16

`fs/fat16.c`, about 2,000 lines, and the only filesystem. Read and write,
create and delete, subdirectories, VFAT long names in UTF-8, one partition.

FAT16 was chosen so the host can read and write the disk image with
`mtools` — no loop device, no root — which is what makes `make write` a
one-liner and what makes the test suites able to verify the guest's writes
with the *host's* tools rather than by reading them back with the same code
that wrote them.

Two structural facts it is easy to get wrong:

**A directory is one of two things, and both have to be carried.** The root
is a fixed run of sectors that cannot grow; every other directory is an
ordinary cluster chain. FAT32 abolished the distinction, FAT16 did not.
`struct dir` with `cluster == 0` meaning the root is how this is said.

**`.` and `..` are the only record of a directory's parent.** A FAT
directory entry says nothing about where it lives, so `mkdir` must write
both or the directory cannot be left. The parent of a directory in the root
is recorded as cluster 0.

The startup script is `/etc/rc` because `rc.local` was not a valid 8.3
name when it was chosen -- a five-character extension. Long names have
made it legal since (task 14); the name stayed.

---

## The terminal

`tty.c` is a terminal, not a driver. `/dev/console` has a list of input
sources and a list of output sinks. `ns16550.c` is a raw serial port
(`/dev/ttyS0`) registered as both; `fbcon.c` is a sink; the keyboard is a
source. `console fb` moves the shell to the screen, `console serial` brings
it back, `console both` mirrors.

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

`poll_char()` masks against the timer, because the tick polls the terminal
too. If it takes a character between a reader checking the pushback slot
and that reader taking one from the device, the two come out in the wrong
order: a line typed `SHELL` arrives as `SHLEL`, rarely enough to be
baffling.

**The line editor moves the cursor with `\r` and `\b` only.** That was
forced when `fbcon.c` understood nothing else; it is a VT102 now, and the
rule stays because it works on any terminal. The redraw is deliberately
minimal: a character typed at the end of a line echoes one character,
because on the framebuffer each one is 128 pixels drawn individually and
redrawing a whole line per keystroke is visibly slow.

---

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

Still absent, each on purpose: window scaling, SACK, timestamps, PAWS, path
MTU discovery, Nagle. All but the last are negotiated options that a peer
works fine without. Nagle is left out because a machine with a 4 KB send
buffer and a human on the other end is not where the forty-byte-header
problem gets solved, and coalescing would make an interactive session
worse.

### On a real LAN

`tools/qemu-net.sh` decides at runtime: an existing bridge if there is one,
otherwise macvtap on a wired interface, otherwise slirp. It has to be
runtime, because **Wi-Fi cannot bridge** — an 802.11 station may only use
its own MAC as the source of frames it sends, so bridging and macvtap are
both impossible on a wireless link. One development machine here is
wireless and the other is wired.

The test suites always use slirp, deliberately: a test that depends on the
building's network is not a test.

---

## Programs

A program is an ELF executable with no extension. **`exec.c` decides what
is executable from the file's first four bytes**, because FAT16 has no
permission bit — do not "tidy" this by adding `.EXE` or by matching on
names.

`lib/` is what a program links against: `crt0.s`, `ulib.c`, `user.ld`.
`system/` is what the system ships, installed into `/BIN`: `ifconfig`,
`ping`, `netstat`, `shutdown`, `env`, `stty`, `resize`, `fsck`. `apps/` is everything else — `cube`,
`fbtest`, `hello`, `fetch`, `httpd`, `spin`, `faulter` — installed at the
root.

That split is recent. The network tools used to be shell builtins reaching
straight into the kernel, which was a layering violation with a
command-line interface.

`ulib` is a thin wrapper over the system calls plus the handful of string
and output helpers that every program needs, and `lib/malloc.c`, a
stand-in allocator. **There is no stdio and no libc.**

`crt0.s` reads `argc` and `argv` at `4(%sp)` and `8(%sp)`, not 0 and 4 —
the kernel enters a program with `jsr`, which pushes a return address
first. Getting that wrong gives a plausible-looking garbage `argc` and a
bus error a few instructions later.

---

## The shell

`shell.c`, about 2,100 lines, running as a task like anything else.

- Environment variables, `export`, `unset`, and inheritance by spawned
  programs
- `PATH`, searched by `spawn_on_path()`
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

106 checks across five suites, all of which boot the machine and drive it
over its serial line:

| | | |
|---|---|---|
| `tests/` | 12 programs | devices: UART, ATA, MFP, RTC, keyboard, SM501 |
| `kernel/fstest.sh` | 39 | the filesystem, verified with the host's mtools |
| `kernel/edittest.sh` | 27 | the editor, history, job control, shutdown |
| `kernel/vmtest.sh` | 15 | what a program cannot touch |
| `kernel/nettest.sh` | 13 | ARP, DHCP, ICMP and TCP against a host web server |

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

Named, so that nobody has to discover them by trying:

**`fork` copies eagerly.** There is no copy-on-write until there is
page-fault handling (task 21), so a `fork` of a large program costs its
whole address space, even when an `execve` follows at once. `spawn` is
the cheap way to start a program.

**Signals are complete except for `sigaltstack`**, refused with `EINVAL`
rather than half supported. `SA_SIGINFO` handlers get Linux/m68k's
`siginfo` and `ucontext`, through the `rt_` calls. A fault's own signal
(SIGSEGV from an access fault, for instance) cannot be caught: the 68040
access-fault frame cannot be redirected to a handler in place.

**Memory is Linux-shaped.** A 256 MB address space with `brk`/`sbrk`,
`mmap`/`munmap`/`mprotect` for anonymous memory and file copies, and
`malloc`/`free`/`calloc`/`realloc` in `lib/malloc.c`, a stand-in until
there is a C library.

**Pipes, `fcntl`, redirection and pipelines work**, for programs and
builtins alike. The shell points its own descriptors 0–2 at the files
for the length of a command and puts them back afterwards, which is
what any shell does for a builtin, and a spawned program inherits them.
A pipeline's programs share a process group, and the terminal's
foreground is a group, so ctrl-C reaches every stage. A background
program that reads the terminal is stopped with SIGTTIN rather than
given keys meant for somebody else.

**No users, no permissions, no `chmod` or `chown`.** FAT16 has nowhere to
put them.

**No symbolic or hard links.** Same reason.

**No shared libraries.** Everything is statically linked.

**No floating-point in the kernel.** Programs may use the FPU; `cube` does.

**Eight tasks, eight descriptors each, one filesystem, one partition, one
network interface.** All static. There is no allocator pressure anywhere in
the kernel because there is nothing dynamic to allocate.

**No paging to disk**, and no demand paging at all.

None of these is hard to fix in isolation. The point of listing them is
that a program expecting any of them will not build, and will not say so
clearly.
