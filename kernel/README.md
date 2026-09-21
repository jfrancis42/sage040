# The Sage040 kernel

A small supervisor-mode kernel: Linux-shaped system calls, a device
driver model, a VFS, a read/write FAT16 filesystem, a terminal with a
line discipline, a clock, a 100 Hz tick, a framebuffer, a text console
on it, and a shell that reaches all of it only through `trap #0`.

It is loaded from the disk by the [boot ROM](../bootrom/), which finds
`KERNEL.ROM` in the filesystem and jumps to it.

```
$ make boot
Sage040 boot ROM
partition 1 at LBA 2048, type 0x06
KERNEL.ROM  43672 bytes, first cluster 2
image SSP = 0x003FFFF0  PC = 0x00000400
starting

Sage040 kernel 0.3  (built Sep 21 2026 10:03:12)
Copyright (C) 2026 Jeff Francis.  GPL-3.0-or-later.

  traps   : 256 vectors at 0x00000000, TRAP #0 is the system call gate
  syscall : TRAP #0, Linux/m68k convention, verified
  cpu     : MC68040, supervisor mode, sr=0x2700 vbr=0x00000000
  fpu     : on-chip, 1/3 = 0.333333
  memory  : 4096 KB, kernel 0x00000000-0x0000b524, stack top 0x003ffff0
  disk    : hda 'QEMU HARDDISK', 204800 sectors (100 MiB)
  clock   : m48t59, 2026-09-21 16:03:14 UTC
  timer   : mfp-timer-d at 99 Hz, HZ=100
  video   : SM501 as /dev/fb0, 640x480x8, double buffered
  network : eth0, 52:54:00:12:34:56
  root    : fat16 on /dev/hda 'SAGE040', 101158 KB, 101074 KB free

kernel ready.  'help' lists commands.

sage$ ls -l
-rw     KERNEL.ROM     43672  2026-09-21 10:03
-rw           CUBE     12472  2026-09-21 10:03
-rw          HELLO     10492  2026-09-21 10:03
-rw         FBTEST     11372  2026-09-21 10:03
4 files, 78008 bytes
sage$ df
volume          type   1K-blocks       used      avail  use%
SAGE040         fat16      101158         84     101074    0%
```

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
```

`make run` skips the boot ROM by loading the ELF with QEMU's `-kernel`.
It is the same kernel either way; `make boot` is the path a real machine
takes. The disk image lives in the project root — see
[`../disk.mk`](../disk.mk).

## The shape of it

```
            shell.c  edit.c        programs that happen to be linked in
   ------------------------------  trap #0
                syscall.c          open read write lseek stat getdents ...
              vfs.c    job.c       descriptors; jobs and signals
        +-----------+-----------+
     fs/fat16.c           dev.c    filesystem types, device registries
        |                   |
   struct blockdev     chardev / netdev / rtcdev / timerdev / fbdev
        |                   |
   drivers/ata.c       drivers/ns16550.c  i8042.c  m48t59.c  mfp.c
                       sm501.c  smc91c111.c
```

The layering is the point, so it is worth saying what it buys:

- **The shell reaches the filesystem, the disk and the terminal only
  through `trap #0`.** Every command in it — `ls`, `cat`, `date`, `fg`,
  `console` — is system calls and nothing else, which is how a claim
  like "programs will run unprivileged later" stays true instead of
  becoming a plan. **`layercheck.sh` enforces it before every link**:
  `shell.c` and `edit.c` may include `syscall.h` and a short list of
  pure headers (`string.h`, `time.h`, `errno.h`) and nothing more. The
  rule was asserted in three documents for months while it was false,
  which is the argument for checking it rather than writing it down.
- **The line editor is above the boundary too.** `edit.c` does what
  readline does: turn off `ICANON` and `ECHO` with `TCSETS`, read
  characters, and do the editing, the history and the searching itself.
  It includes `syscall.h` and nothing else, and it would compile
  unchanged as an ordinary program. A kernel that remembered the last
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

```
exit(1) read(3) write(4) open(5) close(6) unlink(10) time(13) lseek(19)
stime(25) rename(38) times(43) ioctl(54) reboot(88) statfs(99) stat(106)
fsync(118) sysinfo(116) uname(122) getdents(141) nanosleep(162) sync(166)
spawn(400) jobctl(401)
```

Three are not Linux's. `spawn` and `jobctl` are above 400 because Linux
has no such calls — see below. `sync` is 166 rather than Linux's 36,
which would have collided with this table's own use of 36–38. Everything
else is Linux's number exactly.

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

`syscall_dispatch()` takes its pointer arguments at face value. That is
correct while every caller shares the kernel's address space, and it is
the function that will have to validate and copy them when that stops
being true. The MMU is not turned on, so there is no address space to
separate yet.

Nothing else, now. `cmd_console` used to call `tty_sink()` directly —
listing where console output goes was not something a program could ask
for — and three documents asserted the rule while that was quietly
false. It is `TIOCGCONS` and `TIOCSCONS` on the terminal instead, and
`layercheck.sh` runs before every link so the next one fails the build
rather than the documentation.

## Running programs

Anything the shell does not recognise is looked up on the disk and run:

```
sage$ hello one two
hello from a program
  running on Sage040 0.3 (m68040)
  argc = 3
sage$ BIG.TXT
BIG.TXT: not an executable
```

Programs are **ELF32, big-endian, EM_68K**, statically linked at 1 MB --
the toolchain's own output, so there is no flattening step and no private
format. `exec.c` reads the program headers, loads each `PT_LOAD` segment
at its `p_vaddr` and zeroes the part the file did not supply.

**They carry no extension**, and that follows from how executability is
decided. Linux uses a permission bit; a FAT16 volume has none to consult,
so the only thing left is the file itself. The first four bytes say
whether it is a program, which is what Unix has always done. An extension
would be decoration that could lie.

The kernel bounds-checks every segment against the program's window
before reading a byte. With no MMU that check is the only thing between a
mislinked program and the kernel's own memory.

`spawn()` is **not** `execve()`. `execve` replaces the calling process,
and there are no processes to replace: this loads, runs, and returns the
exit status. When there are processes it becomes fork + execve + waitpid
and this call goes away. Naming it `execve` now would cost nothing today
and mislead later.

A program leaves through `exit()`, which unwinds out of however many
frames deep it was, out of the trap it called from, and back into
`exec_spawn` as though it had returned — a longjmp in everything but
name, in `execasm.s`. One program at a time, because that file has room
for one saved context.

See [`../user/`](../user/) for the programs themselves.

## Memory

The MMU is on. Programs run in user mode, in an address space of their
own, and cannot reach the kernel, the devices, or each other.

```
   supervisor (SRP)                    user (URP)
   0x00000000  vectors                 0x10000000  program image
   0x00000400  kernel                  ...         unmapped gap
   ...         all of RAM, identity    0x101f0000  stack, 64 KB
   0x003ffff0  supervisor stack        0x10200000  end
                                       everything else: unmapped
   0xf0000000  SM501 VRAM  ] transparent translation registers,
   0xff000000  I/O         ] supervisor only, uncached
```

The 68040 picks its root pointer from the function code of the access,
not from anything software chooses: supervisor accesses walk **SRP**,
user accesses walk **URP**. That single fact is the design. The kernel's
map is identity over all of RAM, supervisor only, and is built once and
never changed. A program's map is swapped in on every spawn and every
resume, and that swap is the whole of "its own address space".

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

Each program also gets **its own supervisor stack**, and that is not a
detail. Every trap it takes pushes a frame on whatever the supervisor
stack is at the time; if that were the shell's, then the moment the
program stopped and the shell carried on, the shell would grow down over
the very frames the program has to return through. It would look like it
worked -- and for a while it did, with `fg` resuming a program into a
context that had been overwritten.

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

The terminal's idle spin calls `net_poll()` too. A machine that answers
a ping only while waiting for something of its own is not on a network.

### A socket is a file descriptor

`socket.c` gives one `struct file_ops`, so `read()`, `write()` and
`close()` work on a socket without knowing what it is, and a program can
be pointed at one instead of a file. That is also what keeps the TCP
replaceable: a program calls `socket()` and `connect()`, and which
implementation answers is not its business.

Blocking is a spin, through `net_wait()`, which drives the protocol
while it waits -- so the data being waited for can actually arrive. The
same bargain the console has always made, and it becomes a sleep on a
wait queue the day there is a scheduler.

### What TCP does and does not do

The state machine of RFC 793, both opens, retransmission with an
exponentially backed-off timer, and an orderly close. Enough to fetch a
page from a real server and enough to be one.

Not done, each on purpose: **no congestion control** (a machine talking
to its own LAN is not where the internet's congestion is decided);
**no out-of-order reassembly** (a segment arriving ahead of a gap is
dropped and retransmitted, which is legal and costs throughput rather
than correctness); **no window scaling, SACK or timestamps**; and **the
initial sequence number comes from the tick**, which is a real exposure
on a machine facing the open internet and is the sentence to come back
to.

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
the mounted volume — which is honest about a filesystem with one
directory, and does not change the calls above it when that stops being
true.

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
sage$ uptime
0:00:14  (1484 ticks at 100 Hz)
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
itself, which needs an MMU that is not turned on here.

**Only `point()` is required of a driver.** `fb.c` builds `clear`, `line`
and `rect` from it, so a new framebuffer works the moment it can set one
pixel. A driver that has a blitter implements them and those are used
instead: `sm501.c` does its clear and its filled rectangles with the 2D
engine, because at a period-correct clock the CPU cannot clear 640×480
and hold a frame rate — 37 fps against the engine's 50 — while a dozen
short lines cost nothing either way.

Double buffered, so a wireframe drawn a line at a time does not flicker.
`FBIO_FLIP` is one register write, so the change lands between frames.

`user/fbtest` draws one of everything and holds it, which is how a broken
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

The font is the real VGA ROM font rather than a redraw — the
single-storey `g`, the unslashed `0`. See `font8x16.c` for where it came
from and how to reproduce it.

## The terminal

`/dev/console` and `/dev/tty` are `tty.c`, not a UART. A terminal is a
line discipline plus a set of places characters come from and go to; a
UART is one of those places, and so is a screen.

```
sage$ console
output to:
  ttyS0   on
  fbcon   on
input from:
  ttyS0
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
```

Raw mode is Linux's termios, with Linux's numbers: `TCGETS` and `TCSETS`
on descriptor 0, `ICANON` and `ECHO` cleared in `c_lflag`. **`ISIG` is
deliberately left set**, which is what readline does and for the same
reason — turning off every special character looks like "give me
everything" and would mean ctrl-C stopped working, which is the one
thing a terminal must never stop doing.

Two constraints shaped the rest of it.

**There are no cursor escapes.** The framebuffer console understands
carriage return, backspace, tab and newline and drops everything else,
so an editor written with `ESC [ nD` would work perfectly over the
serial line and do nothing at all on the screen. Every movement here is
built from `\r` and `\b`. That turns out to cost nothing: an editor that
never leaves one line does not need more.

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
`VINTR` and `VSUSP` — and raises a signal on the foreground job. That is
all it does: `job_signal_fg()` records a number, because it is called
from interrupt context as often as not. What actually happens to the
program happens in `job_deliver()`, and **where it is called from
decides what it is allowed to do**:

| Site | ctrl-C | ctrl-Z |
|------|--------|--------|
| `JOB_AT_SYSCALL` — the boundary of a system call | yes | yes |
| `JOB_AT_TICK` — the timer interrupt, program in its own code | yes | deferred |

Killing a program means discarding its whole stack. That is safe at a
system call boundary, where the kernel has finished, and safe from the
tick, where the kernel was never involved — but **not** from inside a
system call, where the filesystem might be halfway through a directory
entry. So `syscall.c` counts how deep in the kernel it is and the tick
checks the count before doing anything.

The tick is what makes ctrl-C work at all on a program like `cube`,
which reads no input: nobody is looking at the keyboard while it spins,
so a hundred times a second the tick looks instead. Anything it finds
that is not a signal is pushed back and handed to the next reader in
order.

Stopping is different, because stopping means coming back. `exec_stop()`
and `exec_resume()` in `execasm.s` are a context switch — each saves the
callee-saved registers and the stack pointer in the frame layout
`exec_call` already uses, and jumps to where the other left off. That
works from a system call boundary, which is a C call boundary; it does
not work from an arbitrary instruction, which would need every register
and the program counter saved out of the exception frame. So a program
that makes no system calls can be killed but not stopped, and the signal
simply stays pending until it makes one.

### What does not work yet, and why

`&` and `bg` are parsed, tracked, and refused with a reason. Running a
job in the background means running it while the shell also runs, and
there is nothing to schedule two things. The job goes in the table and
`fg` runs it. Quietly running it in the foreground instead would look
like `&` working, and the person would find out only when their prompt
never came back.

A stopped job holds the program area, so nothing else can start while
one exists — there is one image area at 1 MB and one program stack, and
a second program would load straight over a stopped one. `exec.c`
refuses the spawn rather than leaving that in a comment. It goes away
with an MMU.

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

## The filesystem

FAT16, read and write, registered as the type `fat16` and mounted on
`/dev/hda`. The volume is a genuine MS-DOS one, so the host can put a
file on it with `mcopy` and the kernel reads it, and anything the kernel
writes comes back off the image without the kernel running.

Limits, none of which change a single call: the root directory only, 8.3
names, FAT16 alone. Long-name entries the host wrote are skipped on a
scan rather than misread, so a file created with one is still visible by
its short name and is not damaged. FAT12 and FAT32 are refused at mount
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

`./fstest.sh` boots the kernel on a scratch image, drives a console
session, and then checks the result with `mdir`, `mtype` and `fsck.fat`.
The second half is the part that matters: a filesystem only the kernel
can read would prove nothing. 30 checks.

The device tests in [`../tests/`](../tests/) exercise the same hardware
from bare metal, with no kernel underneath — including `t11-rtc` for the
clock and NVRAM.

## Faults

Every vector except reset lands on one handler, which identifies itself
from the format word the 68040 pushes and reports what it can:

```
*** exception 2: bus error
    pc=0x00003810  sr=0x2700  frame format 7
    d0=00000040  d1=00000004  d2=00000000  d3=00000c10
    a0=00400000  a1=c0de0003  a2=0000070e  a3=00001a7c
    ...
*** panic: unhandled exception
*** halted.
```

Nothing is recoverable yet, so it stops — a silent hang is the one
outcome worth ruling out. That report is what found the first real bug
here: the memory probe walking off the end of RAM, faulting address in
`a0`.

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
cat [FILE...]        print files, or the terminal until ctrl-D
hd FILE              hex dump, first 256 bytes
cp SRC DST           copy a file
mv SRC DST           rename a file
rm FILE...           remove files
stat FILE            size, mode and modification time
df                   space used and available
echo TEXT            print a line
date [-s DATE [TIME]] show or set the clock
uname [-a]           system name, or name and version
uptime               how long the machine has been up
console [NAME on|off] show or change where console output goes
sync                 flush pending writes
free                 physical memory, in pages
ifconfig [A M [G]]   show or set the interface address
dhcp                 ask the network for an address
ping ADDR [N]        ICMP echo
arp / arping ADDR    the address cache
jobs                 list stopped and queued jobs
fg [%N]              run a stopped or queued job
bg [%N]              run one in the background — see Jobs, above
history              the lines remembered so far
halt                 stop the processor

> FILE and >> FILE redirect output
CMD &                queue a job

Anything else is looked up as a program on the disk and run —
including `shutdown`, which stops the machine rather than the
processor.
```

`cat > notes.txt` is how you write a file, because that is how a Unix
user would already do it — which is why there is no "write a file"
command. Errors go to descriptor 2 even when output is redirected.

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
| `exec.c` | the ELF loader, running a program, stopping and resuming one |
| `job.c` | the job table, and where ctrl-C and ctrl-Z land |
| `edit.c` | the line editor and its history — a program, not a kernel facility |
| `timer.c` | jiffies, and sleeping on them |
| `fb.c` | /dev/fb0, and the drawing a driver did not do itself |
| `tty.c` | the line discipline, and the fan-out to sinks |
| `fbcon.c` | /dev/fbcon, the text console |
| `font8x16.c` | the IBM PC font, and where it came from |
| `execasm.s` | the stack switch into a program, the unwind out, and the context switch between |
| `probe.c` | CPU, FPU and memory — the parts with no driver |
| `pmm.c` | the physical page allocator |
| `vm.c` | page tables, address spaces, and turning the MMU on |
| `uaccess.c` | reaching into a program's memory, safely |
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
| `shell.c` | a program, reaching the kernel through `trap #0` (bar one command) |
| `version.c` | the version and build stamp, defined once |
| `uapi.h` | what crosses the system call boundary |
| `drivers/` | `ns16550.c` `i8042.c` `ata.c` `m48t59.c` `mfp.c` `sm501.c` `smc91c111.c` |
| `fs/fat16.c` | FAT16, read and write |
| `fstest.sh` | scripted session, verified with host tools |
| `edittest.sh` | the editor, history, ctrl-C, ctrl-Z, jobs and shutdown |
| `vmtest.sh` | memory protection: what a program cannot touch, and that a bad pointer is an error |
| `nettest.sh` | ARP, DHCP, ICMP and a TCP transfer bigger than the receive buffer |
| `kernel.ld` | vectors at 0, text at 0x400, stack at the top of RAM |
