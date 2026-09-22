# The Sage040 kernel

A small kernel: Linux-shaped system calls, a device driver model, a VFS,
a read/write FAT16 filesystem with subdirectories, an MMU giving every
program an address space of its own, a preemptive round-robin scheduler,
signals and job control, a TCP/IP stack, a terminal with a line
discipline, a clock, a 100 Hz tick, a framebuffer, a text console on it,
and a shell that reaches all of it only through `trap #0`.

This file is about the kernel's internals. [`../os.md`](../os.md)
describes the operating system as a whole, and [`../emacs.md`](../emacs.md)
measures how far short of a real Unix it falls by trying to put GNU Emacs
on it.

It is loaded from the disk by the [boot ROM](../bootrom/), which finds
`KERNEL.ROM` in the filesystem and jumps to it.

```
$ make boot
Sage040 boot ROM
partition 1 at LBA 2048, type 0x06
KERNEL.ROM  108924 bytes, first cluster 2
image SSP = 0x003FFFF0  PC = 0x00000400
starting

Sage040 kernel 0.3  (built Sep 21 2026 15:04:17)
Copyright (C) 2026 Jeff Francis.  GPL-3.0-or-later.

  traps   : 256 vectors at 0x00000000, TRAP #0 is the system call gate
  syscall : TRAP #0, Linux/m68k convention, verified
  cpu     : MC68040, supervisor mode, sr=0x2700 vbr=0x00000000
  fpu     : on-chip, 1/3 = 0.333333
  memory  : 4096 KB, kernel 0x00000000-0x0003c03c, stack top 0x003ffff0
  pages   : 946 of 4 KB free from 0x0003d000 to 0x003ef000
  mmu     : on, 4 KB pages, kernel identity-mapped supervisor-only, 3 pages of tables
  disk    : hda 'QEMU HARDDISK', 204800 sectors (100 MiB)
  clock   : m48t59, 2026-09-21 21:18:04 UTC
  timer   : mfp-timer-d at 99 Hz, HZ=100
  video   : SM501 as /dev/fb0, 640x480x8, double buffered
  fbcon   : /dev/fbcon, 80x30 of IBM PC 8x16, green on black
  keyboard: 8042 as /dev/kbd0, scancode set 1, US layout
  network : eth0, 52:54:00:12:34:56
  console : output to ttyS0 fbcon, input from ttyS0 fbcon kbd0
  net     : eth0 up, ethernet + ARP, no address yet (try `ifconfig`)
  root    : fat16 on /dev/hda 'SAGE040', 101158 KB, 100884 KB free, 2048 byte clusters

kernel ready.  'help' lists commands.

booted from /etc/rc
/$ ls -l
-rw     KERNEL.ROM    108924  2026-09-21 15:05
-rw      NOTES.TXT        42  2026-09-21 14:27
-rw           CUBE     13836  2026-09-21 14:53
-rw          HELLO     11748  2026-09-21 14:53
-rw         FBTEST     12644  2026-09-21 14:53
drw            ETC         0  2026-09-21 15:05
drw            BIN         0  2026-09-21 14:53
-rw          FETCH     12696  2026-09-21 14:53
-rw          HTTPD     12820  2026-09-21 14:53
-rw           SPIN     11596  2026-09-21 14:53
-rw        FAULTER     12668  2026-09-21 14:53
11 files, 196974 bytes
/$ df
volume          type   1K-blocks       used      avail  use%
SAGE040         fat16      101158        274     100884    0%
```

`booted from /etc/rc` is the startup script running, and the prompt is
the working directory: `/$` at the root, `/bin$` inside `/BIN`.

Each device announces itself as its driver registers, so every line is
something the machine actually answered. A startup message assembled at
build time agrees with you while the hardware disagrees.

## Building and running

```bash
make              # kernel.elf and the flat kernel.rom
make run          # run the kernel directly, disk attached -- quickest loop
make install      # write kernel.rom onto the disk as KERNEL.ROM
make boot         # install it, then boot through the boot ROM
make syms         # symbol table, address order
./fstest.sh       # scripted session, checked with the host's own tools
./edittest.sh     # the editor, history, ctrl-C, ctrl-Z, jobs, shutdown
./vmtest.sh       # memory protection, and that a bad pointer is an error
./nettest.sh      # ARP, DHCP, ICMP and a TCP transfer
```

`make run` skips the boot ROM by loading the ELF with QEMU's `-kernel`.
It is the same kernel either way; `make boot` is the path a real machine
takes. The disk image lives in the project root — see
[`../disk.mk`](../disk.mk).

## The shape of it

```
   apps/ system/ lib/            programs, unprivileged, on the disk
            shell.c  edit.c        and two that happen to be linked in
   ------------------------------  trap #0
                syscall.c          open read write lseek stat getdents ...
          uaccess.c    vm.c        a program's pointers, and its address space
       vfs.c  task.c  signal.c     descriptors; tasks, jobs and signals
        +-----------+-----------+
     fs/fat16.c           dev.c    filesystem types, device registries
        |                   |
   struct blockdev     chardev / netdev / rtcdev / timerdev / fbdev
        |                   |
   drivers/ata.c       drivers/ns16550.c  i8042.c  m48t59.c  mfp.c
                       sm501.c  smc91c111.c
```

`net/` sits beside all of that rather than in it: `socket.c` hands the
descriptor table a `struct file_ops` like any other, and everything
below it talks to a `struct netdev`.

The layering is the point, so it is worth saying what it buys:

- **The shell reaches the filesystem, the disk and the terminal only
  through `trap #0`.** Every command in it — `ls`, `cat`, `date`, `fg`,
  `console` — is system calls and nothing else. That was written when
  "programs will run unprivileged later" was still a promise, and the
  promise is what it bought: when the MMU came on and programs really
  did move to the other side of the gate, nothing above the gate had to
  change. **`layercheck.sh` enforces it before every link**:
  `shell.c` and `edit.c` may include `syscall.h` and a short list of
  pure headers (`string.h`, `time.h`, `errno.h`) and nothing more. The
  rule was asserted in three documents for months while it was false,
  which is the argument for checking it rather than writing it down.
- **The line editor is above the boundary too.** `edit.c` does what
  readline does: turn off `ICANON` and `ECHO` with `TCSETS`, read
  characters, and do the editing, the history and the searching itself.
  It includes `syscall.h`, `errno.h` and `string.h` and nothing else,
  and it would compile unchanged as an ordinary program — which is what
  `layercheck.sh` is there to keep true. A kernel that remembered the last
  sixteen things you typed would be a kernel doing a shell's job.
- **The filesystem talks to a `struct blockdev`.** It asks for sector
  2048; it has no idea an ATA taskfile answers. A SCSI controller or a
  RAM disk is a new file in `drivers/` and one more line in `main.c`.
- **The terminal is a `struct chardev` behind descriptors 0, 1 and 2.**
  Replacing the NS16550A, or putting a framebuffer console there
  instead, changes nothing above `drivers/`.
- **`main.c` is the only file that names a chip.** That is deliberate:
  this is a board with parts soldered to it, not a bus that can be
  enumerated, so something has to know what is fitted — and exactly one
  thing does.

## System calls

The convention is Linux/m68k's, unchanged:

```
d0 = call number
d1..d5 = arguments
         trap #0
d0 = result, or a negated errno
```

That is not an imitation. Linux picked the obvious convention for this
architecture and there is nothing to improve on. The numbers are Linux's
i386 numbers — `__NR_write` is 4 — because that is the set most people
recognise.

Forty of them:

```
exit(1) read(3) write(4) open(5) close(6) waitpid(7) unlink(10) chdir(12)
time(13) lseek(19) getpid(20) stime(25) kill(37) rename(38) mkdir(39)
rmdir(40) times(43) ioctl(54) reboot(88) statfs(99) stat(106) sysinfo(116)
fsync(118) uname(122) getdents(141) sched_yield(158) nanosleep(162)
sync(166) getcwd(183) socket(359) bind(361) connect(362) listen(363)
accept(364) sendto(369) recvfrom(371) shutdown(373)
spawn(400) jobctl(401) netctl(402)
```

Four are not Linux's. `spawn`, `jobctl` and `netctl` are above 400
because Linux has no such calls — see below. `sync` is 166 rather than
Linux's 36, which would have collided with this table's own use of
36–40. Everything else is Linux's number exactly.

`ioctl` carries the terminal settings as well as the framebuffer's:
`TCGETS` and `TCSETS` with Linux's `struct termios`, which is what the
shell's line editor uses to turn canonical mode off.

`reboot()` takes `RB_HALT_SYSTEM`, which stops the processor, or
`RB_POWER_OFF`, which asks the board to actually go away — on this
machine through the keyboard controller's reset line. See **Stopping the
machine** below.

Errors are Linux's too, by name and by number: `-ENOENT` is -2 here for
the same reason it is -2 there. There is no global `errno` — a variable
that only makes sense once there are threads to get it wrong. Calls
return a non-negative result or the negated error.

[`uapi.h`](uapi.h) holds what crosses the boundary — `O_CREAT`,
`struct stat`, `struct dirent` — and nothing else. A program gets that
header and never sees `struct fs_type` or the descriptor table, which is
the same split Linux makes under the same name.

`kmain()` makes a call through the gate at startup and checks that an
unknown call number comes back `-ENOSYS`, so the path is known good at
the point it is installed rather than at the point something depends on
it.

### What is still on the wrong side of the line

This section used to say that `syscall_dispatch()` took its pointer
arguments at face value, which was correct while every caller shared the
kernel's address space. It does not any more: the MMU is on, a program's
pointers are not the kernel's, and every one of them goes through
[`uaccess.c`](uaccess.c) — see **Memory** below.

What is left is the shell, which is still a kernel task and so still
passes kernel pointers through the same gate. `uaccess_current()` is
null for it and non-null for a program, which is exactly the
distinction, and it is the one branch that collapses when the shell
moves out of the kernel.

`cmd_console` used to call `tty_sink()` directly —
listing where console output goes was not something a program could ask
for — and three documents asserted the rule while that was quietly
false. It is `TIOCGCONS` and `TIOCSCONS` on the terminal instead, and
`layercheck.sh` runs before every link so the next one fails the build
rather than the documentation.

## Running programs

Anything the shell does not recognise is looked up on the disk and run:

```
/$ hello one two
hello from a program
  running on Sage040 0.3 (m68040)
  argc = 3
  argv[0] = hello
  argv[1] = one
  argv[2] = two
/$ NOTES.TXT
NOTES.TXT: not an executable
```

`PATH` is `/bin:.`, so the system's own tools are found by name and the
current directory is searched after them rather than before.

Programs are **ELF32, big-endian, EM_68K**, statically linked at
`0x10000000` -- the toolchain's own output, so there is no flattening
step and no private format. `exec.c` reads the program headers, loads
each `PT_LOAD` segment at its `p_vaddr` and zeroes the part the file did
not supply.

They used to be linked at 1 MB, and moving them up is not cosmetic. The
kernel identity-maps RAM, so a program at 1 MB sat at an address the
kernel could dereference, and a system call that forgot to go through
`uaccess.c` would quietly read the right bytes instead of failing. At
`0x10000000` there is nothing behind that address in the kernel's map
and the same mistake faults in the first test that touches it.

**They carry no extension**, and that follows from how executability is
decided. Linux uses a permission bit; a FAT16 volume has none to consult,
so the only thing left is the file itself. The first four bytes say
whether it is a program, which is what Unix has always done. An extension
would be decoration that could lie.

The kernel bounds-checks every segment against the user area before
reading a byte. This used to say that with no MMU the check was the only
thing between a mislinked program and the kernel's own memory; that is
history. The MMU protects the kernel at run time now, and what the check
still buys is a clear answer at load time: a segment outside the user
area has no page tables behind it, so refusing it here gives `-ENOEXEC`
instead of a fault partway through loading.

`spawn()` is fork and exec in one call: it starts a new task and
returns its pid. **It does not wait.** Whether to wait is the caller's
decision, and that decision is the whole of what `&` means. `fork()`,
`execve()` and `waitpid()` exist too, with Linux's meanings and status
encoding. `fork` copies the address space eagerly (`vm_clone`), and
`execve` builds the new image before destroying the old, so a failure
returns into an intact caller.

A program leaves through `exit()`, which is `task_exit()`: its
descriptors are closed, its address space and kernel stack are freed by
whoever reaps it, and `waitpid()` in its parent gets the status. There is
no unwinding, because there is nothing to unwind back into — the task
simply stops being scheduled.

See [`../apps/`](../apps/) and [`../system/`](../system/) for the
programs, and [`../lib/`](../lib/) for what they link against: `crt0.s`,
`ulib.c` and `user.ld`.

## Tasks

The machine runs several things at once, preemptively. `ps` shows them.

```
  PID  PPID  STATE  COMMAND
    1     0  run    idle
    2     1  run    sh
    3     2  run    spin calls
```

**The context is the kernel stack pointer, and nothing else.** A task
that is not running is sitting inside `schedule()`, called either
because it blocked or from the tail of an interrupt -- and either way
its registers are already on its own kernel stack. So saving a context
is remembering one pointer and restoring it is putting that pointer
back in A7; the ordinary function epilogue and the ordinary RTE do the
rest.

A brand new task therefore needs a kernel stack that has been made to
look as though it had been suspended that way, and `build_stack()`
fabricates exactly that: an exception frame claiming to come from user
mode, zeroed registers under it, and a return address pointing at the
stub that pops them. The first time it is scheduled it returns into
that stub and RTEs into its entry point, having never run before.

### Where a switch can happen

**Only on the way back to user mode.** The tick marks the running task
as having had its turn; the check happens at the end of an interrupt or
a system call, once the kernel has finished what it was doing.

That single rule is why there is no locking anywhere in this kernel. A
task inside a system call cannot be preempted out of it, so only one
task is ever inside the kernel at a time. The cost is real and worth
knowing: a KERNEL task -- the shell -- is never preempted and must
block or yield, and one that looped without doing either would stop the
machine.

A task may of course give up the processor at any time by blocking,
which is `sleep_on()` rather than preemption, but it is the same switch
underneath.

### Waiting without spinning

Every wait in this system used to be a spin. `wait.h` has queues,
semaphores and mutexes, and the race they exist to close is the one
where a driver's interrupt wakes a queue between a task deciding to
wait and actually sleeping. `sleep_on()` masks across both, so the
window does not exist. A caller always writes:

```c
while (!condition) {
    sleep_on(&queue);
}
```

because a wakeup means "look again", never "it is your turn".

A mutex is a binary semaphore that remembers its owner, and neither can
be used from an interrupt handler -- an interrupt cannot block, so a
handler that must exclude a task masks instead.

### Signals

Any task can signal any user task, and a program can block, ignore or
catch what it chooses: Linux's numbers, the old `sigaction` with
m68k's layout, `sigprocmask`, `sigpending`, `sigsuspend`, `pause` and
`sigreturn`. A handler is started by saving every register, the mask
and the FPU in a frame on the user stack and rewriting the saved
`pt_regs` to return into it. The handler returns through
`__sigreturn_trampoline` in `lib/crt0.s`. See `signal.c`, which also
decides when an interrupted system call is restarted.

Kernel tasks take no signals. They never return to user mode, so one
could never be acted on.

**Delivery is at the boundary, never where the signal is raised.** The
sender sets a bit, because the target may be halfway through a system
call and ending it there would leave the work half done.

ctrl-Z stopping a task is a state, not an unwind: the task stays
exactly where it is, in the middle of whatever call it was making, and
SIGCONT resumes it there. That is only possible because it has a kernel
stack of its own to be left sitting on.

## Memory

The MMU is on. Programs run in user mode, in an address space of their
own, and cannot reach the kernel, the devices, or each other.

```
   supervisor (SRP)                    user (URP)
   0x00000000  vectors                 0x10000000  program image
   0x00000400  kernel, its stack       ...         unmapped gap
   ...         all of RAM, identity    0x1ff00000  stack, 1 MB
                                       0x20000000  end
                                       everything else: unmapped
   0xf0000000  SM501 VRAM  ] transparent translation registers,
   0xff000000  I/O         ] supervisor only, uncached
```

The 68040 picks its root pointer from the function code of the access,
not from anything software chooses: supervisor accesses walk **SRP**,
user accesses walk **URP**. That single fact is the design. The kernel's
map is identity over all of RAM, supervisor only, and is built once and
never changed. A program's map goes with its task and is swapped in by
`schedule()` on every switch to it, and that swap is the whole of "its
own address space". A kernel task has none and runs with whatever was
loaded, which costs nothing: its accesses are supervisor accesses and
walk SRP regardless.

Because the kernel's map is identity, it can reach any physical page by
its address -- which is how it loads an image, builds a program's page
tables and copies a system call's arguments, all without mapping
anything specially.

### A program's pointers are not the kernel's

`0x10001234` is a valid pointer to a program and **an unmapped address
to the kernel**. The user area is deliberately not mapped into the
supervisor space, and the reason is what happens when somebody forgets:
the raw dereference faults immediately, in the first test that touches
that system call. Had the user area been visible, the same omission
would work perfectly until a program passed a bad pointer, and then it
would take the machine down.

So every system call that takes a pointer goes through `uaccess.c`,
which walks the program's page tables in software and returns `-EFAULT`
rather than faulting. A program is allowed to pass rubbish; it gets an
error, and the kernel stays up. `kernel/vmtest.sh` checks that with
four deliberately bad pointers.

The shell is a transitional exception. It runs inside the kernel and
reaches the system through the same gate, so its pointers *are* kernel
pointers; `uaccess_current()` is non-null only while a user program is
running, which distinguishes the two exactly. When the shell moves out
of the kernel, that branch collapses.

### What the MMU actually bought

| | |
|--|--|
| A program cannot read kernel memory | the S bit on every kernel page |
| A program cannot touch the UART or the framebuffer | the transparent translation registers are supervisor-only, so a user access does not match them and falls through to a page table with nothing in it |
| A runaway stack stops | the gap below it is unmapped, not merely unused |
| A bad pointer to a system call is an error | `uaccess.c`, not a fault |
| A kernel stack overflow is caught | an unmapped guard page below each one |
| A program's fault kills the program | and the shell prints why |

### Pages

`pmm.c` hands out 4 KB pages from the end of the kernel to just below
its stack -- a bitmap rather than a free list, so that a use-after-free
corrupts data instead of the allocator. Everything comes from there: a
program's image, its stack, its page tables, and the kernel stack its
system calls run on. `free` in the shell reports it.

Every task also gets **its own supervisor stack**, three pages: a guard
page and then two of stack. That is not a detail. Every trap a task
takes pushes a frame on whatever the supervisor stack is at the time; if
that were shared, then the moment one task stopped and another carried
on, the second would grow down over the very frames the first has to
return through. It would look like it worked -- and for a while it did,
with `fg` resuming a program into a context that had been overwritten.
The guard comes out of the kernel's map rather than the task's, because
it is the kernel that would overflow, running that task's system calls.

## The network

Ethernet, ARP, IPv4, ICMP, UDP, DHCP and TCP, written out rather than
imported. The machine takes a lease from a real DHCP server, answers
ping from other hosts on the LAN, fetches pages from web servers on the
internet, and serves its own files over HTTP to anything that asks.

```
        socket.c        socket(), connect(), accept() -- a socket is an fd
   -------------------
    tcp.c     udp.c     ports, and a state machine
   -------------------
         ip.c           addresses, routing, the checksum
   -------------------
    arp.c    icmp.c     hardware addresses; ping
   -------------------
        net.c           frames in, frames out, the receive ring
   -------------------
   struct netdev        drivers/smc91c111.c
```

**Byte order needs no conversion.** Network order is big-endian and so
is a 68040, so `htons` and `ntohl` are the identity and compile to
nothing. The contrast is with this board's own devices: the ATA data
register and every SM501 register are little-endian, and those quirks
stop at their drivers.

**Unaligned access needs none either.** An IP header starts 14 bytes
into a frame, so its addresses land 2-aligned and never 4-aligned; the
68040 does that in hardware. The structures are still marked packed,
which is about what the compiler would otherwise insert rather than what
the CPU can do.

### Where the protocol code runs, and why there is no locking

One place: `net_poll()`, in ordinary kernel context. The timer interrupt
calls `net_drain()`, which moves frames off the card into a ring and
runs no protocol code at all.

That split is not an optimisation. **The LAN91C111 allocates transmit
buffers from the same pool of packet pages that holds arriving frames**,
so a card whose receiver is never drained stops being able to send. The
first version only polled while waiting for a reply, which worked
perfectly on QEMU's user-mode NAT -- where almost nothing arrives unasked
-- and failed within seconds of meeting a real LAN, with every transmit
returning ENOMEM.

Transmit raises the interrupt mask for its duration, because receive and
transmit share the chip's bank select and pointer register.

`net_poll()` is called by anything that waits on the network --
`net_wait()` and every blocking path in `socket.c` -- so the protocol
runs while a task is waiting for it rather than only when a reply is
expected. That is not the same as running all the time, and a machine
that answers a ping only while waiting for something of its own is not
on a network; the terminal used to call `net_poll()` from its idle spin
for exactly that reason, and the spin went away with the scheduler.
`net.c` still registers `tty_set_idle(net_poll_idle)` and nothing calls
it, which is the loose end here.

### A socket is a file descriptor

`socket.c` gives one `struct file_ops`, so `read()`, `write()` and
`close()` work on a socket without knowing what it is, and a program can
be pointed at one instead of a file. That is also what keeps the TCP
replaceable: a program calls `socket()` and `connect()`, and which
implementation answers is not its business.

Blocking used to be a spin, through `net_wait()`, because there was
nothing else for the processor to do. Now it is a sleep on a wait queue,
bounded at 20 ms so that a missed wakeup costs a small delay instead of
a hang and so that TCP's own timers get looked at whether or not
anything is arriving. `net_wait()` still drives the protocol itself
before each sleep -- the data being waited for has to be able to arrive
-- and it returns early if a signal is pending, which is how ctrl-C
reaches a program waiting on a socket.

### What TCP does and does not do

The state machine of RFC 793, both opens, retransmission with an
exponentially backed-off timer, and an orderly close. Enough to fetch a
page from a real server and enough to be one.

Since then, and each because the one-LAN excuse stopped holding:
**congestion control** (RFC 5681 slow start and congestion avoidance,
halving on loss); **out-of-order reassembly** into a small pool of
shared slots, rather than dropping a segment that arrives ahead of a
gap; **RTT and RTO estimation** with Karn's algorithm, so the
retransmit timer is measured rather than guessed; and **delayed
acknowledgements**.

Not done, still on purpose: **no window scaling, SACK or timestamps** --
without window scaling there is no point going past 32 KB, which is
where `cwnd` is clamped.

The **initial sequence number** used to come from the tick, which was a
real exposure on a machine facing the open internet: an off-path
attacker who can guess an ISN can inject data into a connection. It now
comes from `random.c` plus the tick. `random.c` is not cryptographic and
does not claim to be.

## Devices that are not there

Every driver asks whether its chip is fitted before it touches a
register, with `io_probe8`, `io_probe16` or `io_probe32` from
`memprobe.s` — a read that survives a bus error.

This is not defensive decoration. **An address with no device behind it
raises a bus error; it does not read back zeroes**, on this emulator and
on a real board with an empty socket alike. So a kernel run on a QEMU
built before one of its devices existed does not report a missing
device — it panics inside the first driver that reaches for one, and the
panic looks like a kernel bug rather than a stale emulator. That has
happened once, with the keyboard, which is why the probes exist.

They read and never write, at the width the driver will use, and they
ignore the value: an absent chip and a chip holding zero read the same,
so the only question is whether the bus answered. `main.c` prints what
is missing and carries on, which is why a machine with no disk still
gets a prompt that can tell you there is no disk.

## Devices

Six kinds, each with one interface (see [`dev.h`](dev.h)):

| | |
|---|---|
| `struct chardev` | a byte stream — `/dev/ttyS0`, `/dev/fbcon`, `/dev/fb0` |
| `struct blockdev` | addressable sectors — what a filesystem mounts |
| `struct netdev` | packets — an ethernet interface |
| `struct rtcdev` | seconds since 1970, and nothing else |
| `struct timerdev` | a periodic interrupt — what makes time pass |
| `struct fbdev` | a display: point, and optionally clear, line, rect, flip |

Drivers register themselves during startup and stay registered until the
power goes off. There is no hotplug, no reference counting and nothing
to unregister, because with a handful of soldered-down parts that would
be machinery in search of a problem.

`/dev/console` and `/dev/tty` are the terminal. Path resolution is two
fixed mount points — `/dev` is the device registry, everything else is
the mounted volume, which now walks a tree of its own. That was written
when the volume had one directory and the claim made for it was that it
would not change the calls above it when that stopped being true.
Subdirectories arrived and it did not: `vfs.c` hands the whole path
below `/dev` to the filesystem and never looked at it in the first
place.

## The tick

`drivers/mfp.c` is the board's interrupt controller and its timer. The
MC68901 drives one IPL line and supplies its own vector, so its sixteen
channels arrive at sixteen consecutive vectors — one stub is installed at
all of them and works out which channel it is from the format/vector word
the 68040 pushed.

Timer D at the /200 prescaler runs at 12288 Hz, and a reload of 123
divides that to **99.9 Hz**. `HZ` is 100, which is what Linux used for
most of its life and makes a tick 10 ms — fine enough that a 50 fps frame
is exactly two of them.

```
/$ uptime
0:00:04  (482 ticks at 100 Hz)
```

**A tick faster than its own handler starves everything else.** The
handler returns, the next interrupt is already pending, and no other
instruction ever runs. This machine walked into that once with an MFP
timer at 13 µs, so `mfp_timer_start()` refuses a reload under 8 rather
than accepting it and hanging.

`timer_sleep_ticks()` uses `STOP` rather than spinning, so a sleeping
program costs the host nothing. It returns `-ENODEV` if no timer is
running, because a sleep with nothing to wake it is a hang and an error
is the honest version of that.

It is also the one thing in here that has not caught up with the
scheduler. `STOP` halts the processor, not the task, and the loop runs
in supervisor mode — so the return-to-user check never fires and nothing
else gets a turn for the length of the sleep. `nanosleep()` is the only
caller. What it wants is `sleep_on_timeout()`, which is what everything
in `net/` already uses.

Interrupts are enabled **last** in startup. Until then a fault is
reported by a handler with the console to itself; an interrupt arriving
midway through bringing a driver up would be much harder to understand.

## The framebuffer

`/dev/fb0`, drawn with ioctls:

```c
int fb = open("/dev/fb0", O_RDWR);
ioctl(fb, FBIO_GETINFO, (u32)&info);
ioctl(fb, FBIO_CLEAR, 0);
ioctl(fb, FBIO_LINE, (u32)&line);
ioctl(fb, FBIO_FLIP, 0);
```

Drawing through ioctl rather than through a dozen system calls of its
own: a framebuffer is a device, the device model already carries it, and
putting graphics calls in the system call table would tie the kernel's
ABI to one kind of hardware. Linux controls its framebuffer the same way
— though Linux expects a program to `mmap` the memory and draw for
itself. That used to be out of reach because the MMU was off; it is on
now, and what is missing is `mmap` itself. A program cannot ask for a
mapping of anything, which is the same gap [`../emacs.md`](../emacs.md)
runs into from the other direction.

**Only `point()` is required of a driver.** `fb.c` builds `clear`, `line`
and `rect` from it, so a new framebuffer works the moment it can set one
pixel. A driver that has a blitter implements them and those are used
instead: `sm501.c` does its clear and its filled rectangles with the 2D
engine, because at a period-correct clock the CPU cannot clear 640×480
and hold a frame rate — 37 fps against the engine's 50 — while a dozen
short lines cost nothing either way.

Double buffered, so a wireframe drawn a line at a time does not flicker.
`FBIO_FLIP` is one register write, so the change lands between frames.

`apps/fbtest` draws one of everything and holds it, which is how a broken
driver gets told apart from a broken program that uses one.

## The text console

`/dev/fbcon`: 80 columns by 30 rows of the **IBM PC 8x16 font**, green on
black. 640x480 divided by the character cell, which is exactly the
geometry a VGA text mode had and for exactly the same reason.

**It only writes.** A screen is not an input device. Input arrives
through the terminal layer, from whatever sources it has — see below.

Scrolling is one `copy()` — the SM501's blitter moves 29 rows up in a
single operation. A framebuffer without a blitter leaves `copy` null, and
the console redraws from its own character buffer instead: correct, much
slower, and the reason `copy` is worth implementing in a driver.

**Just enough ANSI to be clearable**: `ESC [ H`, `ESC [ 2J` and
`ESC [ K`, and nothing else. This understood none of them at first, so
anything that wanted to clear the screen had to know which sink it was
talking to — which is the opposite of the point of having sinks.
Cursor addressing is left out on purpose; see **Editing a line** below.

The font is the real VGA ROM font rather than a redraw — the
single-storey `g`, the unslashed `0`. See `font8x16.c` for where it came
from and how to reproduce it.

## The terminal

`/dev/console` and `/dev/tty` are `tty.c`, not a UART. A terminal is a
line discipline plus a set of places characters come from and go to; a
UART is one of those places, and so is a screen.

```
/$ console
output to:
  ttyS0   on
  fbcon   on
input from:
  ttyS0
  kbd0

turn one off with `console NAME off`
```

**Output goes to every enabled sink at once; input is taken from every
source.** So the shell is on the screen and on the serial line
simultaneously, rather than on one of them. `console NAME off` silences
a sink when the duplication is in the way — and the last one cannot be
turned off, because a machine with no console output is one that cannot
tell you why.

That shape is not a preference. The test harnesses drive this machine
over the serial line with `-display none`, and QEMU delivers no keyboard
input at all without a display, so serial has to stay fully live. Making
the console exclusive would break every test in the tree.

`read()` returns one whole line, echoed as it is typed, with backspace
and ctrl-U doing what a person expects and ctrl-D returning 0 for end of
input — canonical mode, in the terminal rather than in a driver, so that
every program that reads a line does not implement it again slightly
differently.

**A read with nothing to read sleeps on a wait queue**, and the tick's
poll is what wakes it. It used to spin, which was fine when there was
nothing else for the processor to do and is not now. This is also what
keeps the machine alive: the shell is a kernel task and kernel tasks are
never preempted, so it has to block or yield, and waiting at the prompt
is where it blocks.

Two translations, named after the termios flags that do the same job:

- **ONLCR** — a newline written out becomes CR + LF, because a terminal
  needs both. A file written through a descriptor gets the bare newline
  it should have.
- **ICRNL** — the carriage return a terminal sends on enter arrives as a
  newline, because that is what C code expects at the end of a line.

**Echo goes to the sinks, not back to the source.** That is why the line
discipline had to leave the UART driver: what you type has to appear on
the screen you are looking at, which is not necessarily the wire the
character arrived on.

A source is any device whose `ioctl` answers `FIONREAD` — that is how
the terminal asks whether a character is waiting without committing to a
read that would block. There are two: the serial port and the keyboard.
Polled, for now; when both are interrupt-driven they should feed one
ring buffer and the poll loop becomes a drain of it, which is a change
inside `tty.c` and nowhere else.

## The keyboard

`drivers/i8042.c` — an Intel 8042 at `0xff700000`, registered as
`/dev/kbd0` and handed to the terminal as an input source. Data at
offset 0, status and command at offset 1: the PC's `0x60`/`0x64` pair
with the gap taken out.

**Scancode set 1, by choice.** A PS/2 keyboard powers up in set 2, where
a release is the prefix `0xF0` then the make code. The 8042 can
translate set 2 to set 1 as it passes — bit 6 of the command byte — and
set 1 is what the PC's own BIOS always saw: a release is the make code
with bit 7 set, no prefix to track.

Worth knowing when reading the driver against a PC reference: **QEMU
resets the controller with translation off**, where a PC arrives with the
BIOS having already turned it on. Code that assumes the PC's state gets
set 2 and decodes nonsense — nonsense that looks like a broken keymap
rather than like the wrong scancode set, which is why `t12-kbd` checks
for `0x1E` and against `0x1C` explicitly.

Shift, caps lock and control are tracked; control produces the control
characters, which is not decoration — without it there is no way to type
the ctrl-U or ctrl-D the line discipline is looking for. The `0xE0`
extended sequences are consumed rather than turned into something
invented, except for right control, which is real.

## Editing a line

The shell's line editor is `edit.c`, and the thing worth knowing about
it is where it is: **above the system call boundary**, exactly where
bash keeps readline. It sets the terminal to raw mode and does the work
itself.

```
ctrl-A  ctrl-E     start and end of the line        Home, End
ctrl-B  ctrl-F     back and forward one character   left, right
ctrl-P  ctrl-N     back and forward in history      up, down
ctrl-R  ctrl-S     search the history               ctrl-G abandons it
ctrl-U  ctrl-K     delete to the start, to the end
ctrl-W             delete the word before the cursor
ctrl-D             delete forwards; end input on an empty line
ctrl-L             clear the screen, line kept and redrawn at the top
```

Raw mode is Linux's termios, with Linux's numbers: `TCGETS` and `TCSETS`
on descriptor 0, `ICANON` and `ECHO` cleared in `c_lflag`. **`ISIG` is
deliberately left set**, which is what readline does and for the same
reason — turning off every special character looks like "give me
everything" and would mean ctrl-C stopped working, which is the one
thing a terminal must never stop doing.

Two constraints shaped the rest of it.

**It moves the cursor with nothing but `\r` and `\b`.** When it was
written the framebuffer console understood carriage return, backspace,
tab and newline and three escape sequences, so an editor written with
`ESC [ nD` would have worked over the serial line and done nothing on
the screen. The console is a VT102 now, but the rule stays: it costs
nothing for an editor that never leaves one line, and it works on any
terminal at all.

**Redrawing is expensive on one of the two sinks.** A character on the
serial line is a byte; on the framebuffer it is 128 pixels drawn
individually. So nothing is redrawn that does not have to be. Typing at
the end of the line — almost everything anyone does — echoes exactly one
character. Moving left is backspaces; moving right re-echoes the
characters passed over, which are already on screen and identical. Only
an edit in the middle rewrites anything, and only as far as the end of
the line.

Arrow keys work from both inputs because `drivers/i8042.c` emits the
same VT100 sequences a serial terminal sends. There is one escape
parser, and it does not know which one it is reading.

If the terminal refuses raw mode, the editor falls back to the kernel's
own line assembly. Nothing does today, but that is the path a pipe or a
file would take.

## Jobs, ctrl-C and ctrl-Z

A job is one command the shell started. They exist because ctrl-C has to
be aimed somewhere, and because `fg %2` has to mean something.

The terminal recognises the interrupt and suspend characters — `c_cc`,
`VINTR` and `VSUSP` — and sends `SIGINT` or `SIGTSTP` to the foreground
task. `signal_char()` in `tty.c` is the whole of it, and it is gated on
`ISIG`, which is why a program that clears `ICANON` and `ECHO` to do its
own line editing still gets its ctrl-C.

**There is one foreground task**, `tty_set_foreground()`, and that is
the entire difference between a job started with `&` and one without: a
background task is not connected to this keyboard in the sense that
matters, so it gets neither character. The terminal is handed over
inside `exec_spawn()` rather than by the shell afterwards, because
loading an image takes long enough to read a disk and a ctrl-C arriving
in that window went to a task that did not exist yet and was simply
lost — which looked exactly like ctrl-C not working.

**Delivery is at one site: `task_ret_to_user()`, on the way back to user
mode.** `signal_send()` only sets a bit and wakes the task if it was
asleep, because the target may be halfway through a system call and
ending it there would leave the filesystem halfway through a directory
entry. A sleeping task's call returns `-EINTR`, the way a real one does,
and the signal is acted on at the boundary it returns through.

The tick is still what makes ctrl-C work at all on a program like
`cube`, which reads no input: nobody is looking at the keyboard while it
spins, so a hundred times a second `tty_poll_signals()` looks instead.
Anything it finds that is not a signal goes in the pushback slot and is
handed to the next reader in order. What has changed is that it only
*raises* the signal — it no longer decides anything about what happens
next.

This section used to describe two delivery sites with different rules —
one at the system call boundary and one in the timer interrupt, with a
depth counter in `syscall.c` deciding what the tick was allowed to do —
because there was no scheduler and no per-task kernel stack to leave a
program sitting on. All of that is gone. There is one site, and it
cannot run while the kernel is in the middle of anything, because it
only runs when the kernel has finished.

What that bought is visible: a bare `spin`, which makes **no system
calls at all**, can now be stopped as well as killed. It used to be that
stopping meant coming back, coming back meant a saved context, and the
only context worth saving was at a C call boundary — so a program that
never called anything could be killed and not stopped. Every task has a
kernel stack of its own now, so ctrl-Z is a state change on it and
SIGCONT resumes it where it stood.

### `&`, `bg` and `fg`

They really run things. This section used to be headed "what does not
work yet": `&` and `bg` were parsed, tracked, and refused with a reason,
because running a job in the background means running it while the shell
also runs and there was nothing to schedule two things. There is now, so
`&` starts the task and the shell does not `waitpid()` on it.

```
/$ spin calls &
[3]  spin calls &
/$ SPINNING-WITH-CALLS
ps
  PID  PPID  STATE  COMMAND
    1     0  run    idle
    2     1  run    sh
    3     2  run    spin calls
/$ jobs
[3]  running  spin calls
```

A stopped job used to hold the one program area at 1 MB, so `exec.c`
refused a second spawn while one existed. That went away with the MMU,
exactly as this document said it would: every task has an address space
of its own, a stopped job keeps its pages, and a new program gets
different ones.

## Stopping the machine

`halt` stops the processor. `shutdown` stops the machine, and how it
does it is a real piece of the hardware rather than an emulator
courtesy: the Intel 8042 has a spare output line which on the IBM PC was
wired to the processor's RESET pin, because there was nowhere else to
put it. Every PC since has rebooted by asking its keyboard controller to
do it, and this board inherited the part and the trick.

`reboot(RB_POWER_OFF)` flushes the filesystem and asks the device model
whether anything can cut the power; `drivers/i8042.c` registered itself
as the thing that can. If nothing had, the kernel says so and halts
instead of pretending.

Under QEMU with `-no-reboot`, a guest-requested reset ends the process —
so the machine stopping and the emulator exiting are the same event.
**Every QEMU invocation in this tree passes `-no-reboot`.** Without it,
the one command whose whole job is getting out would restart the machine
instead.

## Directories

FAT16 has subdirectories and this kernel now uses them: `mkdir`,
`rmdir`, `cd`, `pwd`, and paths like `/etc/rc` that walk the tree.

**A directory is one of two things, and both have to be carried.** The
root is a fixed run of sectors, laid down when the volume was made and
unable to grow. Every other directory is an ordinary cluster chain,
exactly like a file, whose contents happen to be directory entries.
FAT32 abolished the distinction by making the root a chain too; FAT16
did not. `struct dir` with a cluster of 0 meaning the root is how that
is said, and a subdirectory that fills up gets another cluster chained
on while a full root is full for good.

**`.` and `..` are not decoration.** A FAT directory entry records
nothing about where it lives, so `..` is the only record of a
directory's parent anywhere on the volume -- a path walk hitting `..`
reads it from there. A directory made without them cannot be left. The
parent of a directory in the root is written as cluster 0, which is how
FAT spells "the root".

**8.3 names still apply.** `rc.local` is not a valid name here -- five
characters of extension -- which is why the startup script is `/etc/rc`.

**There is one working directory, not one per task.** `cwd` is a static
in `fs/fat16.c`, which was right when the shell was the only thing that
could run. It is the obvious next thing to move into `struct task`: as
it stands, a `chdir()` from any task is a `chdir()` for all of them,
including the shell whose prompt shows it.

## The filesystem

FAT16, read and write, registered as the type `fat16` and mounted on
`/dev/hda`. The volume is a genuine MS-DOS one, so the host can put a
file on it with `mcopy` and the kernel reads it, and anything the kernel
writes comes back off the image without the kernel running.

Limits, none of which change a single call: 8.3 names, FAT16 alone.
"The root directory only" was one of them and is not any more — see
**Directories** above. Long-name entries the host wrote are skipped on a
scan rather than misread, so a file created with one is still visible by
its short name and is not damaged; writing one is not implemented, so
every name this kernel creates goes through `name_to_83()` and a name
that will not fit is refused with `-EINVAL` rather than silently
truncated into a different file. FAT12 and FAT32 are refused at mount
rather than misread as FAT16.

Two things to know before editing `fs/fat16.c`:

- **Every multi-byte field on a FAT disk is little-endian and this CPU
  is not.** Nothing is read by casting a pointer; it all goes through
  `le16()`/`le32()`, which work a byte at a time and so are indifferent
  to alignment as well.
- **A FAT16 volume carries two copies of the table.** Updating only the
  first leaves a disk that works until something checks it. Every FAT
  write goes to all copies.

Files get bare LF line endings. The system is Unix-flavoured and happens
to store its files on an MS-DOS volume; the volume decides the directory
format, not the contents.

## The clock

`drivers/m48t59.c`. The kernel asks it for `time_t` and never sees BCD,
a control register or a two-digit year, so an MC146818 is a new file and
one more line in `main.c`. Calendar arithmetic is in [`time.c`](time.c),
which touches no hardware at all.

The M48T59 has no century register, so the board supplies it and the
part covers 2000–2099. That is a property of the chip, not a shortcut:
every system built around one had to decide this somewhere.

Under QEMU neither the R (freeze) nor W (buffer) control bit does
anything — the model reads live from the host clock and applies each
write immediately, through a normalising conversion. Two consequences,
both measured rather than assumed and both covered by `../tests/t11-rtc`:
reads are taken twice and repeated if the seconds moved, and the date is
written day-first-as-1 so no intermediate state is a date that does not
exist. Writing 29 February in the obvious order silently becomes 1 March.

It also carries **8176 bytes of battery-backed NVRAM**, the only storage
on this machine that survives a power cycle without going through the
disk. `nvram_read()` and `nvram_write()` in `drivers/rtc.h` reach it, and
`rtc_present()` uses a byte of it as the chip's presence test — a dead bus
reads as zeroes, and zeroes are a legal-looking BCD midnight. Nothing else
uses it yet; boot settings are the obvious tenant.

## Testing it

Four scripts, 94 checks between them, and each one boots a real kernel
on a scratch image and drives a console session over the serial line:

| | | |
|---|--:|--|
| `./fstest.sh` | 39 | files, directories, programs, the console |
| `./edittest.sh` | 27 | the editor, history, ctrl-C, ctrl-Z, jobs, shutdown |
| `./vmtest.sh` | 15 | what a program cannot touch, and that a bad pointer is an error |
| `./nettest.sh` | 13 | ARP, DHCP, ICMP and a TCP transfer bigger than the receive buffer |

`fstest.sh` then checks the result with `mdir`, `mtype` and `fsck.fat`,
and that second half is the part that matters: a filesystem only the
kernel can read would prove nothing.

The twelve device tests in [`../tests/`](../tests/) exercise the same
hardware from bare metal, with no kernel underneath — including
`t11-rtc` for the clock and NVRAM. 106 checks in the tree altogether.

Everything any of them writes goes in `scratch/` at the top of the
tree, which `make clean` removes.

## Faults

Every vector except reset lands on one handler, which identifies itself
from the format word the 68040 pushes. **What it does next depends on
where the fault came from**, and the saved SR's supervisor bit is the
whole test.

From **user mode**, it kills the program and nothing else. That is the
first thing the MMU actually bought, and it only works because the
kernel is intact: the exception came from a program, so the kernel's
stack is its own, and the only thing that has to go is the address space
of whatever ran off the end of itself.

```
/$ faulter device
reading the UART at 0xff000000
bus error at 0xff000000, pc=0x100006bc
faulter: segmentation fault
/$ hello after-the-faults
hello from a program
```

The exit status is 139 — 128 plus `SIGSEGV` — which is what a shell
reports for this anywhere else. Note what is *not* done: returning. The
68040 pushes the address of the faulting instruction, so an `rte`
re-runs it and faults again forever. There is no demand paging and no
swap, so ending the program is the only honest answer.

From **supervisor mode** it still panics, because there is nothing else
it could safely do:

```
*** exception 2: bus error
    pc=0x00003810  sr=0x2700  frame format 7
    d0=00000040  d1=00000004  d2=00000000  d3=00000c10
    a0=00400000  a1=c0de0003  a2=0000070e  a3=00001a7c
    ...
*** panic: unhandled exception
*** halted.
```

A silent hang is the one outcome worth ruling out. That report is what
found the first real bug here: the memory probe walking off the end of
RAM, faulting address in `a0`.

The one place a fault *is* handled is `memprobe.s`, which sizes RAM by
writing to each megabyte boundary until one does not answer. The first
address past the end raises a bus error rather than reading back wrong,
so the probe installs its own handler, throws the frame away and returns
as if the test had failed. That is the traditional 68k ROM trick and
still the only way to do it here — nothing on this machine reports how
much RAM is fitted. It is deliberately narrow: safe only because the
routine touches no callee-saved register, and not a general fault
handler.

## Shell commands

```
ls [-l]              list the directory
cd [DIR]             change directory
pwd                  where you are
mkdir DIR...         make directories
rmdir DIR...         remove empty ones
cat [FILE...]        print files, or the terminal until ctrl-D
hd FILE              hex dump, first 256 bytes
cp SRC DST           copy a file
mv SRC DST           rename a file
rm FILE...           remove files
stat FILE            size, mode and modification time
df                   space used and available
free                 physical memory, in pages
echo TEXT            print a line
clear                clear the screen — ctrl-L does it too
test / [ ... ]       ask a question; the exit status answers
source FILE / . FILE run a file of commands
set                  the environment
export NAME=VALUE    put something in it
unset NAME...        take it out
date [-s DATE [TIME]] show or set the clock
uname [-a]           system name, or name and version
uptime               how long the machine has been up
console [NAME on|off] show or change where console output goes
sync                 flush pending writes
ps                   every task on the machine
kill [-SIG] PID      send a signal
jobs                 this shell's jobs
fg [%N]              run a stopped or queued job in the foreground
bg [%N]              run one in the background
history              the lines remembered so far
halt                 stop the processor

< > >> 2> 2>> 2>&1   redirection, for programs and builtins
A | B | C            a pipeline (a builtin only as the first command)
CMD &                run a job in the background
$NAME                expands to what `export` put there

Anything else is looked up on $PATH — which is /bin:. — and run.
The network tools are PROGRAMS, not builtins: `ifconfig`, `ping` and
`netstat` live in /BIN and run unprivileged like anything else, and
`ifconfig dhcp` is what asks the network for an address. So does
`shutdown`, which stops the machine rather than the processor.
```

`ifconfig`, `ping`, `arp` and `dhcp` used to be builtins, which was
honest while the shell was the only thing that could run. They moved out
to [`../system/`](../system/) when programs became able to do the work
themselves, and the move is the point: a network tool that needs nothing
but system calls is a program, and leaving it in the shell would have
made the shell special.

`/etc/rc` runs at startup if it is there — an ordinary script, read by
the same `source` the user can call.

`cat > notes.txt` is how you write a file, because that is how a Unix
user would already do it — which is why there is no "write a file"
command. Errors go to descriptor 2 even when output is redirected.

Redirection is done to the shell's own descriptors 0–2, for the length
of the command, and put back afterwards: a builtin writes through them,
and a program inherits them. Each job is a process group of its own,
and the terminal is handed to that group while the shell waits.

## What is here

| File | |
|------|--|
| `start.s` | entry, vector table, exception and TRAP #0 stubs |
| `main.c` | startup order, and the only file that names a chip |
| `syscall.c` | the call table and the wrappers around the trap |
| `vfs.c` | paths, mounts, the descriptor table |
| `dev.c` | the device registries |
| `time.c` | calendar arithmetic, no hardware |
| `console.c` | how the kernel itself prints, without a descriptor |
| `trap.c` | exception reporting |
| `string.c` | `memcpy` and friends; there is no C library |
| `errno.c` | error numbers as words, for every message the system prints |
| `exec.c` | the ELF loader, and starting a program as a task |
| `edit.c` | the line editor and its history — a program, not a kernel facility |
| `timer.c` | jiffies, and sleeping on them |
| `fb.c` | /dev/fb0, and the drawing a driver did not do itself |
| `tty.c` | the line discipline, and the fan-out to sinks |
| `fbcon.c` | /dev/fbcon, the text console |
| `font8x16.c` | the IBM PC font, and where it came from |
| `probe.c` | CPU, FPU and memory — the parts with no driver |
| `pmm.c` | the physical page allocator |
| `vm.c` | page tables, address spaces, and turning the MMU on |
| `uaccess.c` | reaching into a program's memory, safely |
| `task.c` | the scheduler, and what a task is |
| `taskasm.s` | the context switch, and the way into a new task |
| `wait.c` | wait queues, semaphores, mutexes |
| `signal.c` | raising, and acting on |
| `random.c` | not cryptographic, and says so; TCP's initial sequence numbers |
| `net/net.c` | frames in and out, and the receive ring |
| `net/arp.c` | hardware addresses, and a cache |
| `net/ip.c` | IPv4, and the internet checksum |
| `net/icmp.c` | ping, both directions |
| `net/udp.c` | datagrams and a port table |
| `net/dhcp.c` | asking the network for an address |
| `net/tcp.c` | the state machine, and a window |
| `net/socket.c` | a connection a program can hold |
| `memprobe.s` | reads that survive a bus error, for memory and for absent chips |
| `layercheck.sh` | the layering rule, enforced before every link |
| `shell.c` | a program, reaching the kernel through `trap #0` and nothing else |
| `version.c` | the version and build stamp, defined once |
| `uapi.h` | what crosses the system call boundary |
| `kernel.ld` | vectors at 0, text at 0x400, stack at the top of RAM |
| `drivers/` | `ns16550.c` `i8042.c` `ata.c` `m48t59.c` `mfp.c` `sm501.c` `smc91c111.c` |
| `fs/fat16.c` | FAT16, read and write |
| `fstest.sh` | scripted session, verified with host tools |
| `edittest.sh` | the editor, history, ctrl-C, ctrl-Z, jobs and shutdown |
| `vmtest.sh` | memory protection: what a program cannot touch, and that a bad pointer is an error |
| `nettest.sh` | ARP, DHCP, ICMP and a TCP transfer bigger than the receive buffer |

The programs are not here. [`../lib/`](../lib/) is what they link
against, [`../system/`](../system/) is what goes in `/BIN`, and
[`../apps/`](../apps/) is everything else.
