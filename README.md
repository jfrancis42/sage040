# Sage040

**A virtual 68040 workstation for playing with 68k code.**

Sage Computer Technology built the Sage II and Sage IV in the early 1980s —
plain, boxy 68000 machines with a serial console, a hard disk and no consumer
frills, aimed at people who wanted to get work done rather than play games.
They never built a 68040 machine.

This is a guess at what one might have looked like: a 68040, a serial console,
a disk, a network card, a framebuffer, and a Motorola MFP holding the whole
thing together. It is **not a reproduction of any real hardware** — the Sage
name here is an homage and a design sensibility, not a claim of accuracy.

What it *is* is a machine you can write bare-metal 68k code for, where every
device is a real chip with a datasheet you can download and read.

```
$ qemu-system-m68k -M sage040 -cpu m68040 -m 4 -kernel kernel.elf -serial stdio
```

---

## The design rule

Every emulated device corresponds to a physical part with a publicly available
datasheet. **No virtio, no paravirtual devices, no PCI.** Every chip here is
one you could buy and solder, and the whole machine is a 68040 plus five
memory-mapped peripherals — a board a person could plausibly wire by hand.

That constraint is the point. Code written against this machine is code
written against documented silicon, not against an emulator's conveniences.

| Function | Part | Datasheet |
|---|---|---|
| CPU, FPU, MMU | Motorola **MC68040** | *M68040 User's Manual* |
| Timers, interrupts, 2nd serial | Motorola **MC68901** MFP | *MC68901 Multi-Function Peripheral* |
| Console | National **NS16550A** | *PC16550D* |
| Disk | **ATA taskfile** (WD1003 lineage) | T13 ATA/ATAPI |
| Ethernet | SMSC **LAN91C111** | *LAN91C111* |
| Video | Silicon Motion **SM501** | *SM501* |

Parts were chosen for how little driver you need to write. ATA in polled PIO
is eight registers and no DMA. The LAN91C111 keeps packets in its own FIFO
with no descriptor rings in host memory, which makes it dramatically easier to
drive than a SONIC or a LANCE. The NS16550A is the most thoroughly documented
UART ever made.

---

## What you need

Three things, in this order:

1. **A `m68k-elf` cross toolchain** — compiler, assembler, linker, debugger.
2. **A patched QEMU**, because the machine and one of its chips do not exist
   upstream.
3. This repository.

---

## 1. The toolchain

Targets `m68k-elf`: bare metal, no host operating system, no C library. Built and
tested against binutils 2.45, GCC 15.2.0 and GDB 17.1, though nothing depends
on those exact versions.

Some distributions package it — on Arch, the AUR has `m68k-elf-binutils`,
`m68k-elf-gcc` and `m68k-elf-gdb`. Otherwise it is a routine from-source
build of about twenty minutes:

```bash
export PREFIX="$HOME/m68k/install" TARGET=m68k-elf
export PATH="$PATH:$PREFIX/bin"

# binutils: as, ld, objdump, objcopy, nm, readelf, size, ar
../binutils-2.45/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --disable-werror
make -j$(nproc) && make install

# GCC: C only, no target libc
../gcc-15.2.0/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j$(nproc) all-gcc all-target-libgcc
make install-gcc install-target-libgcc

# GDB
../gdb-17.1/configure --target=$TARGET --prefix="$PREFIX" \
    --with-python=/usr/bin/python3 --disable-nls --disable-werror
make -j$(nproc) all-gdb && make install-gdb
```

Full instructions, flags, and how to confirm the FPU is real rather than
soft-float: **[`toolchain.md`](toolchain.md)**.

Compiler flags this tree uses:

```make
-mcpu=68040 -ffreestanding -nostdlib -nostdinc -O2 -Wall -Wextra \
-fno-builtin -fno-stack-protector
```

---

## 2. The emulator

The machine needs a patched QEMU. Three of the four changes are small; one is
a device model written from scratch.

### What the patch adds

**`hw/m68k/sage040.c`** — the machine itself. Mostly wiring: every device
except the MFP already existed in QEMU's tree.

**`hw/misc/mc68901.c`** — a **new MC68901 MFP device model**. QEMU had none.
It implements all 24 registers at their datasheet offsets, the 16-channel
vectored interrupt controller (enable, pending, in-service and mask registers,
correct priority order, vector generation, and both automatic and software
end-of-interrupt modes), timers A–D in delay mode with all seven prescaler
ratios and live readable down-counters, timers A and B in event-count mode
counting edges on the TAI/TBI inputs, the GPIP parallel port with direction
and edge-select, and the USART against a QEMU chardev.

Not modelled, and logged when touched: pulse-width timer modes, and the
USART's synchronous modes, sync character, break generation and parity.

**`target/m68k`** — an optional interrupt-acknowledge callback. The MFP is a
*vectored* interrupt controller, which upstream QEMU had no way to support;
its own comment in `op_helper.c` notes that "real hardware gets the interrupt
vector via an IACK cycle at this point" and that nothing emulated relied on
it. A vectored controller does: the MFP clears the acknowledged channel's
pending bit and, in software-EOI mode, flags it in service. Without the
callback every interrupt repeats forever. About fifteen lines.

**`hw/display/sm501.c`** — guard the PCI variant with `#ifdef CONFIG_PCI`.
Upstream compiles it unconditionally, so a board that uses only the sysbus
variant fails to link unless the entire PCI subsystem is dragged in. This
board has no PCI bus and should not carry one.

### Building it

Against QEMU 11.1.1:

```bash
curl -LO https://download.qemu.org/qemu-11.1.1.tar.xz
tar xf qemu-11.1.1.tar.xz
Q=$PWD/qemu-11.1.1

cp qemu-patch/new-files/hw-m68k-sage040.c          $Q/hw/m68k/sage040.c
cp qemu-patch/new-files/hw-misc-mc68901.c          $Q/hw/misc/mc68901.c
cp qemu-patch/new-files/include-hw-misc-mc68901.h  $Q/include/hw/misc/mc68901.h
patch -d $Q -p1 < qemu-patch/sage040.patch

mkdir build && cd build
$Q/configure --target-list=m68k-softmmu \
    --prefix=$HOME/m68k/sage040-qemu --enable-slirp \
    --disable-docs --disable-werror --disable-guest-agent \
    --disable-tools --disable-vnc --disable-spice
ninja && ninja install
```

Needs `meson` and `ninja`. Confirm PCI stayed out with
`grep CONFIG_PCI build/m68k-softmmu-config-devices.mak`, which should print
nothing.

Details and rationale: **[`qemu-patch/README.md`](qemu-patch/README.md)**.

---

## 3. Running something

```bash
cd tests && make run
```

That builds eleven bare-metal test programs and runs each one, exercising every
device on the machine:

```
 test programs passed: 11
 test programs failed: 0
```

Or boot the whole machine — the ROM finds `KERNEL.ROM` on the disk, the
kernel comes up, and you get a prompt:

```bash
make boot
```

A full invocation looks like:

```bash
qemu-system-m68k -M sage040 -cpu m68040 -m 4 \
    -kernel kernel.elf \
    -serial file:out.txt \
    -chardev file,id=mfpusart,path=usart.txt,input-path=usart.in \
    -serial chardev:mfpusart \
    -display none -no-reboot \
    -drive file=disk.img,format=raw,if=ide \
    -nic user,model=smc91c111
```

Serial 0 is the 16550 console; serial 1 is the MFP's USART.

Two things that will waste your time otherwise.

**Choose the serial option to match how you are driving it.** Interactively
in a terminal, `-serial stdio` is fine and `-serial mon:stdio` additionally
gives you the monitor on ctrl-A c. For a *scripted* session, neither will
do: `mon:stdio` does not forward piped stdin at all, so feed the guest with
`-chardev stdio,id=con,signal=off -serial chardev:con`, and note that
anything sent before the guest opens the port is discarded when it clears
the UART's receive FIFO. For capture only, `-serial file:`.

**`STOP` halts the CPU but not QEMU**, so scripts should watch the output
for a sentinel and kill it, as `tests/runtest.sh` does.

### Running at period speed

By default QEMU runs as fast as the host allows, which is roughly 150x a real
68040. `-icount` fixes the instruction rate and makes virtual time
deterministic:

```bash
qemu-system-m68k -M sage040 -cpu m68040 -m 4 -icount shift=6,sleep=on ...
```

`shift=6` is 15.6 M instructions/sec, in the region of a 25 MHz 68040 — the
speed the part launched at in 1990. It is an order-of-magnitude model, not
cycle accuracy: every instruction is charged the same time, and there is no
cache or memory-latency modelling.

---

## Memory map

| Base | Size | Device | Access | Endianness |
|------|------|--------|--------|------------|
| `0x00000000` | `-m` (default 4 MB) | RAM — vectors at 0, code at `0x400` | any | big |
| `0xf0000000` | 16 MiB | SM501 video memory | any | big (plain RAM) |
| `0xff000000` | 8 | NS16550A UART | byte | n/a |
| `0xff100000` | 16 | ATA command block | byte, 16-bit data | **little** for data |
| `0xff101000` | 2 | ATA control block | byte | n/a |
| `0xff200000` | 16 | LAN91C111 | byte and 16-bit | native (big) |
| `0xff300000` | 24 | MC68901 MFP | byte | n/a |
| `0xff400000` | 2 MiB | SM501 control registers | **32-bit only** | **little** |
| `0xff600000` | 8 KiB | M48T59 clock + NVRAM | byte | — |

Endianness is not uniform, and it is the single biggest source of bugs. The
ATA data register and every SM501 register are little-endian; everything else
matches the CPU.

**Booting** loads a big-endian ELF32 (`EM_68K`) with `-kernel` and enters at
the ELF entry point with SP at the top of RAM, supervisor mode, interrupts
masked. No ROM, no bootloader, and deliberately no bootinfo block — the OS is
expected to know its own machine.

**Interrupts** all arrive through the MFP, which drives IPL 6 and supplies its
own vector. Peripheral interrupt lines are wired to its GPIP pins: the UART on
GPIP5 (channel 7), ATA on GPIP4 (channel 6), ethernet on GPIP3 (channel 3).
Two of those pins double as the MFP's timer event inputs, which is a property
of the real chip — timer A can count disk interrupts directly.

---

## Writing code for it

There are two kinds, and it is worth being deliberate about which one you
are writing.

**Bare metal** — your code *is* the machine. Loaded by `-kernel` or by the
boot ROM, entered in supervisor mode with interrupts masked, owning every
register. The tests, the boot ROM and `cube/` are all like this.

**A program** — the kernel is running and owns the hardware, and you reach
it through `trap #0`: file descriptors, a filesystem, a terminal, a
framebuffer, a clock. `user/` is like this, and a program touches no
registers at all.

**[`programmer-guide.md`](programmer-guide.md)** is the reference for both.
Sections 1–14 are the hardware: boot protocol, linker script, a minimal
`crt0`, the full MFP register and channel map, worked driver code for every
device, the interrupt handler pattern including how to recover the vector
from the 68040 exception frame, MMU bring-up, and a gotchas section.
**Section 15** is writing a program: the system call ABI, what a program
gets, files, the terminal, the framebuffer and time.

The short version of the gotchas:

- **Access width is enforced.** SM501 control registers are 32-bit only; the
  MFP and UART are byte registers. A wrong-width access can land in the wrong
  byte lane and produce no output and no error at all. When a new driver is
  silent, suspect access width first.
- **Empty delay loops vanish at `-O2`.** Increment a `volatile`.
- **Do not out-run your interrupt handler.** A timer fast enough to fire
  before the handler finishes livelocks the machine. 1–10 ms is a sane
  scheduler tick.
- **A disabled MFP channel loses interrupts**, it does not defer them.
- **Q12 fixed-point rotation destroys unit vectors.** Scale a normal to
  4096 before rotating it, or every face of your cube will test as facing
  away and nothing will be drawn — with no error anywhere.

---

## What's here

| Path | |
|---|---|
| `README.md` | this file |
| `programmer-guide.md` | how to write code for the machine |
| `toolchain.md` | building the cross toolchain |
| `design.md` | why the machine is shaped the way it is |
| `qemu-patch/` | the emulator changes, reproducible from pristine source |
| `tests/` | eleven bare-metal device tests, `make run` |
| `cube/` | a rotating wireframe cube on bare metal — a hardware benchmark |
| `bootrom/` | a boot ROM that finds `KERNEL.ROM` on the disk and runs it |
| `kernel/` | the kernel: system calls, drivers, VFS, FAT16, shell |
| `user/` | programs that run on it — `cube`, `hello`, `fbtest` |
| `types.h` | the integer types, shared by the hardware header, the kernel and the ABI |
| `disk.mk` | the machine's hard disk, shared by everything that touches it |
| `boot/` | a 78-byte proof-of-life kernel, for checking the toolchain before building the emulator (runs on stock QEMU's `virt`, not Sage040) |

`tests/` doubles as a support library for the **bare-metal** side: `crt0.s`,
`sage040.ld`, a 16550 console driver and the MFP interrupt plumbing are
shared by the boot ROM and `cube/`. The kernel deliberately does not use
it — it has its own drivers, and builds with `-DSAGE040_NO_TESTLIB` so
those names cannot collide with a driver's own helpers.

From the top level:

```bash
make            # boot ROM, kernel and programs
make boot       # put them on the disk and boot the machine
make run        # the kernel without the boot ROM in the way
make programs   # just rebuild what is in user/ onto the disk
make cube       # the bare-metal cube demo
make test       # the device tests, then the kernel's own test
make tests      # just the device tests
make fstest     # just the kernel's test
make disk-ls    # partition table and directory listing
make disk-fsck  # check the filesystem with the host's tools
make clean      # build artifacts, keeping the disk
make distclean  # also remove the disk image
```

### The test suite

Every device has a bare-metal test that exercises the real hardware path —
nothing is stubbed. 11 programs, all passing.

| Test | What it proves |
|---|---|
| `t1-cpu` | Supervisor mode, VBR, 32×32→64 multiply, and the FPU against exact IEEE-754 bit patterns for pi, 1/3 and `fsqrt(2)` |
| `t2-uart` | Divisor latch behind DLAB, and a local-loopback run of six byte patterns through the real datapath |
| `t3-ata` | `IDENTIFY DEVICE`, capacity, then a 512-byte write and read-back compared byte for byte |
| `t4-net` | A real ARP request transmitted, and the reply parsed |
| `t5-mmu` | Real three-level page tables, one page remapped to a different frame, verified in both directions |
| `t6-irq` | UART IRQ → MFP GPIP5 → channel 7 → vector `0x47` → handler → `RTE` |
| `t7-mfp-irq` | Priority, vector generation, enable gating, pending/in-service semantics, masking, software EOI and in-service inhibition |
| `t8-mfp-timers` | All four timers, prescaler ratios measured by racing two timers, and event-count mode counting real ATA interrupts |
| `t9-mfp-usart` | Transmit verified against the output file, receive verified against fed-in bytes |
| `t10-sm501` | Device ID, register endianness, 16 MiB with no aliasing, a 640×480 framebuffer filled and read back |
| `t11-rtc` | NVRAM is real memory and does not alias onto the clock, the oscillator advances, a written date reads back, and 30 February rolls into 1 March |

### Booting from disk

`bootrom/` is a boot ROM that mounts a **real MS-DOS disk**, finds
`KERNEL.ROM` in the root directory, loads it to address 0 and runs it — using
the image's own 68000 reset vectors to find its stack pointer and entry point.

```bash
make disk           # 100 MB hd.img in the project root: MBR + FAT16 partition
make -C kernel install   # build the kernel, mcopy it in as KERNEL.ROM
make boot           # ROM mounts the filesystem and boots it
make disk-ls        # partition table and directory listing
```

The disk is genuinely DOS-formatted, so the host reads and writes it with
ordinary tools and no root:

```bash
mcopy -i hd.img@@1M kernel.rom ::/KERNEL.ROM
mdir  -i hd.img@@1M ::/
fsck.fat -n -v hd.img@@1M
```

Replacing the kernel is a file copy, not a `dd` at a magic offset. Needs
`mtools`, `dosfstools` and `util-linux`.

The disk image lives in the project root, because it belongs to the machine
rather than to any one program that touches it: the ROM boots from it, the
kernel reads and writes it, the host puts files on it. `disk.mk` holds its
definition and every Makefile includes that.

`make -C bootrom write-cube` puts the cube on the disk in place of the kernel,
which is the demonstration the loader was first written against and a useful
way to prove it without the kernel in the picture.

### The kernel

`kernel/` is a small supervisor-mode kernel: Linux-shaped system calls, a
device driver model, a VFS, a read/write FAT16 filesystem, a terminal with a
line discipline, a clock, and a shell that reaches all of it only through
`trap #0`.

```
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
```

Each device announces itself as its driver registers, so every line is
something the machine actually answered.

**System calls follow Linux.** The convention is Linux/m68k's, unchanged —
call number in `d0`, arguments in `d1`–`d5`, result or a negated errno back
in `d0` — and the numbers and error values are Linux's too, because
`__NR_write` being 4 is a fact a lot of people already carry around.

**Devices sit behind a driver model.** Six kinds — a character device, a
block device, a network device, a clock, a periodic timer and a framebuffer
— each with exactly one interface, and nothing above them names a chip. The filesystem asks a `struct blockdev` for a sector and
has no idea an ATA taskfile answers; the shell writes to a descriptor and has
no idea an NS16550A is on the other end. Swapping either is a new file in
`kernel/drivers/` and one more line in `main.c` — which is the only file in
the kernel that names a part at all.

**There is a 100 Hz tick, a framebuffer, and a text console on it.** The
MC68901's timer D drives `nanosleep()`; the SM501 is `/dev/fb0` and draws
through ioctls; `/dev/fbcon` puts 80×30 of the IBM PC 8×16 font on it, in
green. Console output goes to the screen **and** the serial line at once
— the terminal has a list of sinks, not a current one — so the serial log
stays complete whatever the display is doing.

**The shell is a program that happens to be linked in.** It includes
`syscall.h` and nothing else from the kernel: not the VFS, not the device
layer, not the console. It cannot reach a chip even by accident, so "programs
will run unprivileged later" stays a true statement rather than becoming a
plan.

**And it runs programs off the disk.** Anything the shell does not recognise
is looked up, loaded and run:

```
sage$ hello one two
hello from a program
  running on Sage040 0.3 (m68040)
  argc = 3
sage$ uptime
0:00:14  (1484 ticks at 100 Hz)
sage$ cube
cube: 640x480x8 on fb0, Q12 fixed point, 50 fps
press any key to stop
cube: 251 frames in 5 seconds (50 fps)
```

The cube is a program now, not a bare-metal demo: it opens `/dev/fb0`,
draws with ioctls, and paces itself with `nanosleep()` against a 100 Hz
tick from the MC68901. It includes no hardware header at all — the
include path does not offer one.

Programs are ordinary ELF32 executables — the toolchain's own output, no
flattening step — and they carry **no extension**. That follows from how
executability is decided: Linux uses a permission bit and a FAT16 volume has
none, so the only thing left to consult is the file itself. The kernel reads
the first four bytes. `CUBE`, not `CUBE.EXE`, and a text file named
`CUBE.EXE` would still be refused.

See [`user/README.md`](user/README.md).

The filesystem is read **and** write. Because the volume is a genuine MS-DOS
one, the host can drop a file on it and the kernel reads it, and anything the
kernel writes comes back off the image afterwards without the kernel running.
`kernel/fstest.sh` checks precisely that, with `mtype`, `mdir` and `fsck.fat`
— a filesystem only the kernel can read would prove nothing.

See [`kernel/README.md`](kernel/README.md).

### The cube

`cube/` is a rotating 3D wireframe cube — rotation, perspective projection,
hidden-line removal and Bresenham line drawing, all computed live, double
buffered in the SM501's video memory and cleared by its 2D engine. It exists
because a machine is not real until something runs on it.

![cube](cube/docs/cube1.png)

---

## Status

The machine is finished and every device is proven by test. On top of it there
is now a small kernel — console, system calls, disk, filesystem, shell — which
is the beginning of an operating system rather than a port of one. Bare metal
still works and is still the point: the test suite and the cube run with no
kernel underneath them at all.

What is not there yet: preemption, more than one program at a time, user
mode, a keyboard, and a TCP/IP stack. The ethernet
driver exists and registers `eth0`, but nothing above it sends a packet
yet. `design.md` tracks what is decided and what is not.

---

## License

Copyright (C) 2026 Jeff Francis.

This program is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License, version 3 or later**, as
published by the Free Software Foundation. It comes with NO WARRANTY. See
[LICENSE](LICENSE) for the full text.

**One exception.** `qemu-patch/` is QEMU-derived work and stays
`GPL-2.0-or-later`, matching upstream. That is deliberate: GPL-2.0-or-later
can be used under GPL-3, so it sits happily inside this project, but
relicensing it to GPL-3 would make it impossible to ever offer upstream —
QEMU cannot take GPL-3 code. Each file in that directory carries its own SPDX
header.
