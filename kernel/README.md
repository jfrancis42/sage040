# The Sage040 kernel

A small supervisor-mode kernel: Linux-shaped system calls, a device
driver model, a VFS, a read/write FAT16 filesystem, a terminal with a
line discipline, a clock, and a shell that reaches all of it only
through `trap #0`.

It is loaded from the disk by the [boot ROM](../bootrom/), which finds
`KERNEL.ROM` in the filesystem and jumps to it.

```
$ make boot
Sage040 boot ROM
partition 1 at LBA 2048, type 0x06
KERNEL.ROM  36960 bytes, first cluster 2
image SSP = 0x003FFFF0  PC = 0x00000400
starting

Sage040 kernel 0.2  (built Sep 21 2026 08:26:25)
Copyright (C) 2026 Jeff Francis.  GPL-3.0-or-later.

  traps   : 256 vectors at 0x00000000, TRAP #0 is the system call gate
  syscall : TRAP #0, Linux/m68k convention, verified
  cpu     : MC68040, supervisor mode, sr=0x2700 vbr=0x00000000
  fpu     : on-chip, 1/3 = 0.333333
  memory  : 4096 KB, kernel 0x00000000-0x00009954, stack top 0x003ffff0
  disk    : hda 'QEMU HARDDISK', 204800 sectors (100 MiB)
  clock   : m48t59, 2026-09-21 14:26:35 UTC
  network : eth0, 52:54:00:12:34:56
  root    : fat16 on /dev/hda 'SAGE040', 101158 KB, 101120 KB free

kernel ready.  'help' lists commands.

sage$ df
volume          type   1K-blocks       used      avail  use%
SAGE040         fat16      101158         38     101120    0%
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
                shell.c            a program that happens to be linked in
   ------------------------------  trap #0
                syscall.c          open read write lseek stat getdents ...
                 vfs.c             paths, mounts, the descriptor table
        +-----------+-----------+
     fs/fat16.c           dev.c    filesystem types, device registries
        |                   |
   struct blockdev     struct chardev / netdev / rtcdev
        |                   |
   drivers/ata.c       drivers/ns16550.c  m48t59.c  smc91c111.c
```

The layering is the point, so it is worth saying what it buys:

- **`shell.c` includes `syscall.h` and nothing else from the kernel.**
  Not `vfs.h`, not `dev.h`, not `console.h`. It cannot reach the
  filesystem or a chip even by accident, which is how a claim like
  "programs will run unprivileged later" stays true instead of becoming
  a plan.
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
stime(25) rename(38) ioctl(54) reboot(88) statfs(99) stat(106)
fsync(118) uname(122) getdents(141) sync(166)
```

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

## Devices

Four kinds, each with one interface (see [`dev.h`](dev.h)):

| | |
|---|---|
| `struct chardev` | a byte stream — the console, and the `/dev` names |
| `struct blockdev` | addressable sectors — what a filesystem mounts |
| `struct netdev` | packets — an ethernet interface |
| `struct rtcdev` | seconds since 1970, and nothing else |

Drivers register themselves during startup and stay registered until the
power goes off. There is no hotplug, no reference counting and nothing
to unregister, because with a handful of soldered-down parts that would
be machinery in search of a problem.

`/dev/console` and `/dev/tty` are the terminal. Path resolution is two
fixed mount points — `/dev` is the device registry, everything else is
the mounted volume — which is honest about a filesystem with one
directory, and does not change the calls above it when that stops being
true.

## The terminal

`drivers/ns16550.c` is a terminal, not just a UART, and the difference
is the line discipline: `read()` returns one whole line, echoed as it is
typed, with backspace and ctrl-U doing what a person expects and ctrl-D
returning 0 for end of input. That is canonical mode, and it belongs in
the driver for the same reason it belongs in the tty layer on a real
system — otherwise every program that reads a line implements it again,
slightly differently.

Two translations, named after the termios flags that do the same job:

- **ONLCR** — a newline written out becomes CR + LF, because a terminal
  needs both. A file written through the same `write()` gets the bare
  newline it should have.
- **ICRNL** — the carriage return the terminal sends on enter arrives as
  a newline, because that is what C code expects at the end of a line.

Polled in both directions, on purpose: a polled console works before
interrupts are set up, works inside a panic, and cannot deadlock against
the code reporting the fault. The chip's interrupt already reaches MFP
channel 7; moving to it means changing `tty_read()` to take from a ring
buffer, and nothing above the driver moves.

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

It also carries **8176 bytes of battery-backed NVRAM**, which is the only
storage on this machine that survives a power cycle without going through
the disk. Nothing uses it yet.

## Testing it

`./fstest.sh` boots the kernel on a scratch image, drives a console
session, and then checks the result with `mdir`, `mtype` and `fsck.fat`.
The second half is the part that matters: a filesystem only the kernel
can read would prove nothing. 16 checks.

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
date [-s DATE TIME]  show or set the clock
uname [-a]           system name, or name and version
sync                 flush pending writes
halt                 stop the machine

> FILE and >> FILE redirect output
```

`cat > notes.txt` is how you write a file, because that is how a Unix
user would already do it — which is why there is no "write a file"
command. Errors go to descriptor 2 even when output is redirected.

## What is here

| File | |
|------|--|
| `start.s` | entry, vector table, exception and TRAP #0 stubs |
| `memprobe.s` | one memory test that survives a bus error |
| `main.c` | startup order, and the only file that names a chip |
| `syscall.c` | the call table and the wrappers around the trap |
| `vfs.c` | paths, mounts, the descriptor table |
| `dev.c` | the device registries |
| `time.c` | calendar arithmetic, no hardware |
| `console.c` | how the kernel itself prints, without a descriptor |
| `trap.c` | exception reporting |
| `string.c` | `memcpy` and friends; there is no C library |
| `probe.c` | CPU, FPU and memory — the parts with no driver |
| `shell.c` | a program, reaching the kernel only through `trap #0` |
| `version.c` | the version and build stamp, defined once |
| `uapi.h` | what crosses the system call boundary |
| `drivers/` | `ns16550.c` `ata.c` `m48t59.c` `smc91c111.c` |
| `fs/fat16.c` | FAT16, read and write |
| `fstest.sh` | scripted session, verified with host tools |
| `kernel.ld` | vectors at 0, text at 0x400, stack at the top of RAM |
