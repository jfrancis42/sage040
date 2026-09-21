# The Sage040 kernel

A small supervisor-mode kernel for the Sage040 machine: a console, a
system call gate, an ATA disk driver, a read/write FAT16 filesystem, and
a shell to drive them by hand.

It is loaded from the disk by the [boot ROM](../bootrom/), which finds
`KERNEL.ROM` in the filesystem and jumps to it.

```
$ make boot
Sage040 boot ROM
partition 1 at LBA 2048, type 0x06
KERNEL.ROM  22988 bytes, first cluster 2
image SSP = 0x003FFFF0  PC = 0x00000400
starting

Sage040 kernel 0.1  (built Sep 21 2026 07:44:15)
Copyright (C) 2026 Jeff Francis.  GPL-3.0-or-later.

  traps   : 256 vectors at 0x00000000, TRAP #0 is the system call gate
  syscall : TRAP #0 gate, 4 calls, verified
  cpu     : MC68040, supervisor mode, sr=0x2704 vbr=0x00000000
  fpu     : on-chip, 1/3 = 0.333333
  memory  : 4096 KB, kernel 0x00000000-0x00006008, stack top 0x003ffff0
  console : NS16550A at 0xff000000, 8N1, scratch register verified
  mfp     : MC68901 at 0xff300000, 16 vectored channels on IPL 6
  disk    : ATA, 'QEMU HARDDISK', 204800 sectors (100 MiB)
  network : LAN91C111 rev 0x3391, MAC 52:54:00:12:34:56
  video   : SM501, device id 0x050100a0, 16 MiB at 0xf0000000
  fs      : FAT16 'SAGE040', 101158 KB, 101134 KB free, 2048 byte clusters

kernel ready.  'help' lists commands.

sage>
```

Every line of that inventory is a device the kernel touched during
startup, not a list assembled at build time. A startup message that
reports what was assumed agrees with you while the hardware disagrees,
which is worse than printing nothing.

## Building and running

```bash
make              # kernel.elf and the flat kernel.rom
make run          # run the kernel directly, disk attached -- quickest loop
make install      # write kernel.rom onto the disk as KERNEL.ROM
make boot         # install it, then boot through the boot ROM
make syms         # symbol table, address order
./fstest.sh       # scripted filesystem test, checked with host tools
```

`make run` skips the boot ROM by loading the ELF with QEMU's `-kernel`.
It is the same kernel either way; `make boot` is the path a real machine
takes.

The disk image lives in the project root and is shared by the boot ROM,
the kernel and the host — see [`../disk.mk`](../disk.mk).

## What is here

| File | |
|------|--|
| `start.s` | entry, vector table, exception and TRAP #0 stubs |
| `memprobe.s` | one memory test that survives a bus error |
| `main.c` | startup order |
| `version.c` | the version and build stamp, defined once |
| `console.c` | NS16550A driver: `kputc`, `kputs`, `kgetc`, `kgets` |
| `trap.c` | exception reporting and the system call dispatcher |
| `ata.c` | ATA taskfile disk, polled PIO |
| `fs.c` | FAT16, read and write |
| `string.c` | `memcpy` and friends; there is no C library |
| `probe.c` | the startup hardware inventory |
| `shell.c` | the console shell |
| `fstest.sh` | scripted filesystem test, verified with host tools |
| `kernel.ld` | vectors at 0, text at 0x400, stack at the top of RAM |

## Supervisor and user mode

The kernel runs in supervisor mode from the first instruction in
`start.s` and never leaves it. There are no user programs yet, but the
boundary they will cross already exists: **`TRAP #0`** is the system call
gate.

```
d0 = call number    SYS_PUTC, SYS_PUTS, SYS_GETC, SYS_GETS
d1 = first argument
d2 = second argument
                    trap #0
d0 = result
```

`kmain()` makes one call through the gate at startup and checks the
answer, so the path is known to work at the point it is installed rather
than at the point something first depends on it.

Two things are deliberately not done yet, and both are listed in
[`../design.md`](../design.md): the MMU is not turned on, so there is no
address space to separate, and `syscall_dispatch()` takes its pointer
arguments at face value. That is correct while the only caller is the
kernel itself. It is the function that will have to validate and copy
them once an unprivileged program in its own address space can call it.

## The console

Polled in both directions. That is a starting point chosen on purpose,
not an omission: a polled console works before interrupts are set up,
works inside a panic, and cannot deadlock against the code reporting the
fault. The UART's interrupt already reaches MFP channel 7, so an
interrupt-driven receive path is a change inside `console.c` — `kgetc()`
will take from a ring buffer instead of the line status register, and
nothing above it moves.

`kgets()` gives a line editor a serial terminal can actually use:
backspace and DEL rub out, Ctrl-U kills the line, CR and LF both end it,
and a full buffer refuses more rather than overrunning.

## The filesystem

FAT16, read and write, on the disk the boot ROM booted from. The volume
is a genuine MS-DOS one, so the host can put a file on it with `mcopy`
and the kernel reads it, and anything the kernel writes can be pulled off
the image afterwards without the kernel running.

```c
int  fs_open(const char *name, int flags);   /* O_READ O_WRITE O_CREATE
                                                O_TRUNC O_APPEND       */
s32  fs_read(int fd, void *buf, u32 len);
s32  fs_write(int fd, const void *buf, u32 len);
s32  fs_seek(int fd, s32 offset, int whence);
int  fs_close(int fd);

int  fs_unlink(const char *name);
int  fs_rename(const char *from, const char *to);
int  fs_stat(const char *name, struct fs_dirent *out);
int  fs_readdir(int index, struct fs_dirent *out);
u32  fs_free_bytes(void);
```

Current limits, none of which change those calls: the root directory
only, 8.3 names, FAT16 alone. Long-name entries written by the host are
skipped on a scan rather than misread, so a file created with one is
still visible by its short name and is not damaged.

Two things in `fs.c` are worth knowing about before editing it:

- **Every multi-byte field on a FAT disk is little-endian and this CPU is
  not.** Nothing is read by casting a pointer; it all goes through
  `le16()`/`le32()`, which work a byte at a time and so are indifferent to
  alignment as well.
- **A FAT16 volume carries two copies of the table.** Updating only the
  first leaves a disk that works until something checks it. Every FAT
  write goes to all copies.

There is no real-time clock on this machine, so every timestamp written
is a fixed one. A stamp that is wrong but constant is better than one
that is wrong and varies, because it is obviously synthetic rather than
quietly plausible. An MC146818 would fix it and is on the list in
`../design.md`.

### Testing it

`./fstest.sh` boots the kernel on a scratch image and drives a console
session, then checks the result with `mdir`, `mtype` and `fsck.fat`. The
second half is the part that matters: a filesystem only the kernel can
read would prove nothing. What is checked is that a file the kernel wrote
comes back byte for byte through mtools, that a file the host wrote is
what the kernel printed, and that `fsck.fat` finds nothing to complain
about afterwards.

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

Nothing is recoverable yet, so it stops. A silent hang is the one outcome
worth ruling out, and that report is what found the first real bug here:
the memory probe walking off the end of RAM, with the faulting address
sitting in `a0`.

The one place a fault *is* handled is `memprobe.s`, which sizes RAM by
writing to each megabyte boundary until one does not answer. The first
address past the end of memory raises a bus error rather than reading
back wrong, so the probe installs its own bus error handler, throws the
frame away and returns as if the test had simply failed. That is the
traditional 68k ROM trick and still the only way to do it here — nothing
on this machine reports how much RAM is fitted.

## Shell commands

```
ls              list the root directory
cat FILE        print a file
hd FILE         hex dump the first 256 bytes
write FILE      type a file in; a lone '.' ends it
append FILE     the same, added to the end
cp FROM TO      copy a file
mv FROM TO      rename a file
rm FILE         delete a file
stat FILE       size, attributes, first cluster
free            space used and available
echo TEXT       print a line
ver             kernel version
halt            stop the machine
```

The shell is not a design goal; it is how the console and filesystem
calls get exercised by hand. Every command is a direct call into `fs.c`
or `console.c` and nothing else, so a command that misbehaves points at
the layer underneath rather than at itself. When user programs exist it
becomes one of them, reaching the same calls through `TRAP #0`.
