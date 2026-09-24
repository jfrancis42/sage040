# Sage040, and SuckOS

**A virtual 68040 workstation, and a Unix-like system that runs on it.**

Sage Computer Technology built the Sage II and Sage IV in the early 1980s —
plain, boxy 68000 machines with a serial console, a hard disk and no consumer
frills, aimed at people who wanted to get work done rather than play games.
They never built a 68040 machine.

**Sage040** is a guess at what one might have looked like: a 68040, a serial
console, a disk, a network card, a framebuffer and a Motorola MFP holding the
whole thing together. It is **not a reproduction of any real hardware** — the
Sage name is an homage and a design sensibility, not a claim of accuracy.

**SuckOS** is the operating system written for it: protected address spaces,
preemptive multitasking, demand paging and swap, signals and job control, an
ext2 filesystem, a TCP/IP stack, a framebuffer console, and a
C library that ordinary POSIX programs build against, with threads,
pseudo-terminals and a real terminfo database. It is **multi-user**:
the console and ssh both ask for a name and a password, hashes live in
`/etc/shadow`, and every path a system call takes is checked against
the file's owner, group and mode. **CPython 3.14**
runs on it -- with TLS, SQLite, compression and readline behind it -- and
so do GNU bash, sed, grep, less, the one true awk, uEmacs, vi, ssh,
rsync and 98 of suckless's utilities, each built from its own unmodified
upstream source.

**It assembles and links its own programs**: GNU binutils runs on the
machine, and the C library, the start files and the linker script are
installed on its disk.

```
$ make boot
SuckOS 0.3 on Sage040  (built Sep 22 2026 13:04:11)
/$ uname -a
SuckOS sage040 0.3 m68040 built Sep 22 2026 13:04:11
/$ bash
bash-5.3$ for f in *.txt; do wc -l "$f"; done | sort -n | tail -3
```

Bare metal still works and is still the point: the device tests and the cube
demo run with no kernel underneath them at all.

---

## The design rule

Every emulated device corresponds to a physical part with a publicly available
datasheet. **No virtio, no paravirtual devices, no PCI.** Every chip here is
one you could buy and solder, and the whole machine is a 68040 plus a handful
of memory-mapped peripherals — a board a person could plausibly wire by hand.

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
| Clock and NVRAM | ST **M48T59** | *M48T59* |
| Keyboard | Intel **8042** | *8042 controller* |

Parts were chosen for how little driver you need to write. ATA in PIO is
eight registers and no DMA. The LAN91C111 keeps packets in its own FIFO with
no descriptor rings in host memory. The NS16550A is the most thoroughly
documented UART ever made.

---

## Getting it running

Three things, in this order: a cross toolchain, a patched QEMU, and this
repository. **[`toolchain.md`](toolchain.md)** has the full commands; the
short form is:

```bash
# 1. m68k-elf toolchain (binutils, gcc, gdb) into ~/m68k/install
#    -- see toolchain.md

# 2. the emulator: QEMU 11.1.1 plus this machine and its MFP
curl -LO https://download.qemu.org/qemu-11.1.1.tar.xz && tar xf qemu-11.1.1.tar.xz
cp qemu-patch/new-files/hw-m68k-sage040.c          qemu-11.1.1/hw/m68k/sage040.c
cp qemu-patch/new-files/hw-misc-mc68901.c          qemu-11.1.1/hw/misc/mc68901.c
cp qemu-patch/new-files/include-hw-misc-mc68901.h  qemu-11.1.1/include/hw/misc/mc68901.h
patch -d qemu-11.1.1 -p1 < qemu-patch/sage040.patch
# configure --target-list=m68k-softmmu --prefix=$HOME/m68k/sage040-qemu ...

# 3. the C library, once, and then the system
make libc                  # picolibc for this machine, into ~/m68k/sage040-libc
make boot                  # build everything, put it on hd.img, boot it
```

`make boot` prints the banner above and leaves you at a shell. `shutdown`
stops the machine; with no display, `shutdown -h` and closing the window do
the same.

**`make boot` puts the SYSTEM on the disk and nothing else.** The ported
programs are built and installed separately, because each one fetches and
builds its own source and that takes time you may not want spent:

```bash
make world      # the system, every port, and Python: everything
make ports      # the ports alone: bash, sed, grep, awk, less, sbase,
                # ncurses, uemacs, vi
make python     # Python alone -- 45 MB and 2,244 files, minutes to copy
make programs   # system/ and apps/ alone, as `make boot` does
```

A single port, if that is all you want:

```bash
make -C ports/bash install
```

---

## The machine

| Base | Size | Device | Access | Endianness |
|------|------|--------|--------|------------|
| `0x00000000` | `-m` (64 MB here) | RAM — vectors at 0, code at `0x400` | any | big |
| `0xf0000000` | 16 MiB | SM501 video memory | any | big (plain RAM) |
| `0xff000000` | 8 | NS16550A UART | byte | n/a |
| `0xff100000` | 16 | ATA command block | byte, 16-bit data | **little** for data |
| `0xff101000` | 2 | ATA control block | byte | n/a |
| `0xff200000` | 16 | LAN91C111 | byte and 16-bit | native (big) |
| `0xff300000` | 24 | MC68901 MFP | byte | n/a |
| `0xff400000` | 2 MiB | SM501 control registers | **32-bit only** | **little** |
| `0xff600000` | 8 KiB | M48T59 clock + NVRAM | byte | — |
| `0xff700000` | 4 KiB | Intel 8042 keyboard | byte | — |

Endianness is not uniform, and it is the single biggest source of bugs. The
ATA data register and every SM501 register are little-endian; everything else
matches the CPU.

**Booting** loads a big-endian ELF32 (`EM_68K`) with `-kernel` and enters at
the ELF entry point with SP at the top of RAM, supervisor mode, interrupts
masked. No ROM, no bootloader, and deliberately no bootinfo block — the system
is expected to know its own machine. `bootrom/` is a boot ROM that mounts the
ext2 filesystem, finds `KERNEL.ROM` and runs it, which is how the machine
boots from disk.

**Interrupts** all arrive through the MFP, which drives IPL 6 and supplies its
own vector. Peripheral interrupt lines are wired to its GPIP pins: the UART on
GPIP5 (channel 7), ATA on GPIP4 (channel 6), ethernet on GPIP3 (channel 3),
the clock on GPIP2 (channel 2) and the keyboard on GPIP1 (channel 1). Two of those pins double as the MFP's timer
event inputs, which is a property of the real chip — timer A can count disk
interrupts directly.

**[`design.md`](design.md)** says why the machine is shaped this way, and
**[`qemu-patch/README.md`](qemu-patch/README.md)** covers the emulator changes.

---

## The system

**[`os.md`](os.md) describes SuckOS in full.** In one paragraph: Linux-shaped
system calls (the numbers, the errnos and the `trap #0` convention are
Linux/m68k's), a preemptive scheduler where `nice` sets the length of a turn,
per-process address spaces with demand paging and a swap file, signals with
job control and sessions, a VFS over a read/write ext2 (FAT16 is still there,
for a disk from somewhere else),
a terminal with several input sources and output sinks, a VT102 framebuffer
console, a TCP/IP stack written out rather than imported, and a cryptographic
random generator.

Programs run **unprivileged**, each in its own address space at `0x10000000`.
One that touches a register, a kernel address or a null pointer takes a bus
error and is killed; the shell says what happened and prompts again. A pointer
handed to a system call is walked in software and refused with `EFAULT`
rather than faulting.

Programs can be linked against **picolibc** (`libc/`), statically or against
`/lib/libc.so` through `/lib/ld.so`, so that every process shares one copy of
the library's text. **[`libc/README.md`](libc/README.md)** covers the port and
the POSIX layer added to it.

### What runs on it

| | |
|---|---|
| `ports/python` | CPython 3.14.7: big integers, the FPU, threads, sockets, curses, the standard library on the disk |
| `ports/bash` | GNU bash 5.3.20 — job control, arrays, `[[ ]]`, arithmetic, here-documents |
| `ports/ncurses` | ncurses 6.5: the terminfo database at /usr/share/terminfo, and curses |
| `ports/less` | the pager, over terminfo |
| `ports/zlib` | zlib 1.3.1 |
| `ports/openssl` | OpenSSL 3.5.4: TLS, and the digests `hashlib` uses |
| `ports/sqlite` | SQLite 3.53.4, library and shell |
| `ports/bzip2`, `ports/xz`, `ports/zstd` | the compressors, and `bz2`/`lzma`/`compression.zstd` |
| `ports/readline` | GNU readline 8.3, for Python's prompt |
| `ports/libffi` | libffi 3.5.2 — calls built at run time, closures included |
| `ports/dropbear` | ssh, sshd and `dropbearkey`; public-key authentication |
| `ports/rsync` | rsync 3.4.1, over ssh |
| `ports/binutils` | **binutils 2.45 that RUNS ON THE MACHINE**: `as`, `ld`, `ar`, `nm`, `objdump`, `strip`, `readelf` |
| `ports/gmp`, `ports/mpfr`, `ports/mpc` | the arithmetic libraries a compiler needs |
| `ports/libstdcxx` | the C++ standard library, for the target |
| `ports/sbase` | 98 POSIX utilities: `sort`, `find`, `xargs`, `tar`, `make`, `ed`, `bc`, `wc`, `cut`, `tr`, the checksums |
| `ports/sed`, `ports/grep` | GNU sed 4.10 and GNU grep 3.12 |
| `ports/awk` | the one true awk |
| `ports/uemacs`, `ports/vi` | uEmacs/PK and neatvi |
| `system/` | the system's own programs: `ifconfig`, `ping`, `netstat`, `route`, `arp`, `host`, `ntpdate`, `fsck`, `swapon`, `nvram`, `irqs`, `stty`, `df`, `id`, `who`, `w`, `uptime`, `sh` |
| `apps/` | demonstrations and test programs: `cube`, `httpd`, `fetch`, `fbmap`, `pagetest` |

---

## Writing code for it

There are two kinds, and it is worth being deliberate about which one you
are writing.

**Bare metal** — your code *is* the machine. Loaded by `-kernel` or by the
boot ROM, entered in supervisor mode with interrupts masked, owning every
register. `tests/`, `bootrom/` and `cube/` are like this.

**A program** — the kernel is running and owns the hardware, and you reach it
through `trap #0` or, more usually, through the C library: file descriptors, a
filesystem, a terminal, sockets, a framebuffer, a clock.

**[`programmer-guide.md`](programmer-guide.md)** is the reference for both:
the boot protocol, a minimal `crt0`, the register maps and worked drivers for
every device, the interrupt handler pattern, MMU bring-up — and then the
system call ABI, what a program gets, and how to build one.

The short version of the hardware gotchas:

- **Access width is enforced.** SM501 control registers are 32-bit only; the
  MFP and UART are byte registers. A wrong-width access lands in the wrong
  byte lane and produces no output and no error at all.
- **Empty delay loops vanish at `-O2`.** Increment a `volatile`.
- **Do not out-run your interrupt handler.** A timer fast enough to fire
  before the handler finishes livelocks the machine.
- **A disabled MFP channel loses interrupts**, it does not defer them.

---

## What's here

| Path | |
|---|---|
| `README.md` | this file |
| `os.md` | the operating system, in full |
| `design.md` | why the machine is shaped this way |
| `progress.md` | what is still to be built |
| `programmer-guide.md` | how to write code for the machine, bare metal or hosted |
| `toolchain.md` | building the cross toolchain, the emulator and the C library |
| `qemu-patch/` | the emulator changes, reproducible from pristine source |
| `tests/` | twelve bare-metal device tests, and the support library they share |
| `cube/` | a rotating wireframe cube on bare metal — a hardware benchmark |
| `bootrom/` | the boot ROM: finds `KERNEL.ROM` on the filesystem and runs it |
| `kernel/` | the kernel, and the scripted test suites that drive it |
| `libc/` | picolibc for this machine, its POSIX layer, and the tests |
| `ldso/` | the dynamic linker, `/lib/ld.so` |
| `lib/` | the small library programs written for this system link against |
| `system/` | the system's own programs, installed into `/bin` |
| `auth/` | login, su, sudo, passwd, useradd, userdel, and the account files |
| `apps/` | everything else, installed at the disk root |
| `ports/` | programs written by other people: bash, sbase, sed, grep, awk, uEmacs, vi |
| `tools/` | `qemu-net.sh`, which decides how the guest reaches the network |
| `/tmp/scratch` | everything the test suites write -- outside the tree on purpose; `make clean` removes it, `SAGE_SCRATCH` moves it |
| `types.h`, `disk.mk`, `machine.conf` | the integer types, the machine's disk, and its RAM and disk sizes -- shared by everything |

---

## Building and running

```bash
make            # boot ROM, kernel and programs
make boot       # put them on the disk and boot the machine
make run        # the kernel without the boot ROM in the way
make programs   # rebuild system/ and apps/ onto the disk
make libc       # picolibc, once
make cube       # the bare-metal cube demo
make disk-ls    # partition table and directory listing
make clean      # build artifacts, keeping the disk
make distclean  # also remove the disk image
```

The disk is a genuine ext2 image, so the host reads and writes it with
e2fsprogs and no root -- `tools/fsimg.sh` is the one place that knows how to
reach the filesystem inside the partition:

```bash
tools/fsimg.sh hd.img put kernel.rom /KERNEL.ROM
tools/fsimg.sh hd.img ls-l /
tools/fsimg.sh hd.img fsck
```

Underneath, that is e2fsprogs' `?offset=` suffix on the device name, which
every one of its tools understands, so nothing is ever extracted with `dd`
and nothing needs a loop device:

```bash
debugfs "hd.img?offset=1048576"
e2fsck -fn "hd.img?offset=1048576"
```

### Networking

`tools/qemu-net.sh` decides at runtime how to attach the guest: an existing
bridge if the host has one, a macvtap if its primary interface is wired, and
QEMU's user-mode NAT otherwise — which is what a laptop gets, because an
802.11 station may only source frames from its own MAC and so cannot bridge.
`SAGE_NET=slirp|macvtap|bridge|lan|none` overrides it. The test suites always
use NAT, deliberately: they need no privileges and touch nothing outside the
emulator.

---

## Tests

Everything here is tested, and the tests are the reason to trust any of it.
`make test` runs them all: **31 suites**, about two hours -- the machine
is a 25 MHz 68040, and one of the suites waits for the wall clock.

| | |
|---|---|
| `make tests` | the twelve bare-metal device tests — every device, no stubs |
| `make cryptotest` | the kernel's ChaCha20 and BLAKE2s against the RFC vectors |
| `make fstest` `make fscktest` | the filesystem, checked with the host's own MS-DOS tools |
| `make edittest` | the line editor, history, jobs, scripts, `shutdown` |
| `make vmtest` `make pagetest` | memory protection, demand paging, swap |
| `make nettest` `make lotest` `make dnstest` `make tcptest` | the network, loopback, the resolver, TCP |
| `make libctest` `make sotest` | picolibc, the POSIX layer, shared libraries |
| `make devtest` | interrupts, the NVRAM, the limits, `mmap` of the framebuffer |
| `make threadtest` `make ptytest` | threads and futexes; pseudo-terminals |
| `make curstest` `make lesstest` | terminfo and curses; less |
| `make logtest` `make crontest` | the kernel's log; cron |
| `make awktest` `make sedtest` `make greptest` `make sbasetest` `make bashtest` | the ported programs |
| `make pytest` | CPython, every answer against the host's Python |
| `make uemacstest` `make vitest` | the editors |
| `make bashsuite` | every one of bash's own 83 tests (hours, not minutes) |

Two habits run through all of them. **Check against something
independent**: a file the machine wrote is read back with the host's debugfs
and the whole volume is checked with `e2fsck`, which shares no line of code
with the driver;
a checksum is compared with the host's `sha256sum`, awk's and grep's own
upstream test tables are the answers, and a ported program's output is
compared with the same source built for the host. And **every fix gets a
negative control**: the change is reverted, the suite is run again, and if
nothing fails the test was not testing anything.

---

## License

Copyright (C) 2026 Jeff Francis.

This program is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License, version 3 or later**, as
published by the Free Software Foundation. It comes with NO WARRANTY. See
[LICENSE](LICENSE) for the full text.

**Two exceptions.** `qemu-patch/` is QEMU-derived work and stays
`GPL-2.0-or-later`, matching upstream: GPL-2.0-or-later can be used under
GPL-3, so it sits happily inside this project, but relicensing it to GPL-3
would make it impossible to ever offer upstream. And the parts of `libc/`
that are linked into programs that are not GPL — the picolibc backend, the
network layer and termcap — are BSD-licensed for that reason. Each file
carries its own SPDX header.

No source from any ported program is copied into this tree: each `ports/*`
fetches its own, at a pinned version, and applies the patches kept beside it.
