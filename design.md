# Sage040 — design

A 68040 workstation built entirely from **real, datasheet-backed silicon**. No
virtio, no paravirtual devices, no PCI. Every part below is a chip you could
buy and solder, chosen so the design could plausibly be built in hardware.

Implemented as a custom QEMU machine, `sage040`.

- **[`programmer-guide.md`](programmer-guide.md)** — how to write code for it
- [`qemu-patch/`](qemu-patch/) — the emulator, reproducible from pristine source
- [`tests/`](tests/) — eleven device tests, `make run`
- [`cube/`](cube/) — a rotating wireframe cube; the first real program on the machine
- [`toolchain.md`](toolchain.md) — the cross toolchain

---

## 1. Device inventory

| Function | Part | Why this part |
|---|---|---|
| CPU, FPU, MMU | Motorola **MC68040** | On-chip FPU and a working paged MMU |
| Timers, interrupts, 2nd serial | Motorola **MC68901 MFP** | The classic 68k companion chip; one part gives timers, a vectored interrupt controller, a parallel port and a USART |
| Console | National **NS16550A** | The most thoroughly documented UART ever made; a working console is ~20 lines |
| Disk | **ATA taskfile** (WD1003 lineage) | Eight registers, polled PIO, no DMA or descriptors — ~60 lines for read and write |
| Ethernet | SMSC **LAN91C111** | On-chip packet FIFO with **no descriptor rings in host memory**, which is what makes it far easier than a SONIC or LANCE |
| Video | Silicon Motion **SM501** | Plain linear framebuffer in 16 MiB of its own memory |
| Keyboard | Intel **8042** | The PC/AT controller, memory mapped; two registers and a scancode stream |
| Clock, NVRAM | ST **M48T59** TIMEKEEPER | Directly memory-mapped byte registers, no index/data port pair, and 8 KiB of battery-backed SRAM alongside |

Selection criterion throughout: **fewest registers to poke**. Deliberately
rejected — NCR 53C94 SCSI (bus phases, CDBs, DMA), DP83932 SONIC (descriptor
areas plus a CAM load), Zilog Z8530 (fiddlier than a 16550 for no gain).

**The clock is an M48T59 and not an MC146818**, which was the first
candidate. The MC146818 is the better-known part and the easier one to
buy today, but QEMU's model of it is an **ISA device** and this board has
no ISA bus; using it would mean either inventing one or rewriting the
upstream model's parent type, and a large invasive patch is exactly what
`qemu-patch/` is meant not to contain. The M48T59 is already a sysbus
device upstream, so it cost no emulator code at all, its registers are
memory-mapped bytes like everything else here rather than an index/data
port pair, and it brings 8 KiB of non-volatile RAM — the only storage on
this machine that survives a power cycle without going through the disk.

---

## 2. Memory map

| Base | Size | Device | Interrupt |
|------|------|--------|-----------|
| `0x00000000` | `-m`, default 4 MB | RAM — vectors at 0, code at `0x400` | — |
| `0xf0000000` | 16 MiB | SM501 video memory | — |
| `0xff000000` | 8 | NS16550A UART | MFP channel 7 |
| `0xff100000` | 16 | ATA command block | MFP channel 6 |
| `0xff101000` | 2 | ATA control block | — |
| `0xff200000` | 16 | LAN91C111 | MFP channel 3 |
| `0xff300000` | 24 | MC68901 MFP | drives **IPL 6**, vectored |
| `0xff400000` | 2 MiB | SM501 control registers | — |
| `0xff600000` | 8 KiB | M48T59 NVRAM; the clock is its last 8 bytes | MFP channel 2 |
| `0xff700000` | 4 KiB | Intel 8042: data at +0, status/command at +1 | MFP channel 1 |

Boot protocol: a big-endian ELF32 (`EM_68K`) loaded with `-kernel`, entered at
its ELF entry point with SP at the top of RAM. No ROM, no bootloader, and no
bootinfo block — the OS knows its own machine.

**Booting from disk.** `bootrom/` is loaded that way and then loads everything
else itself: it reads the partition table, mounts the FAT16 volume, finds
`KERNEL.ROM` in its root directory and loads it to address 0. The image carries
a 68k vector table at its start, so the loader takes the initial SSP from
offset 0 and the entry point from offset 4 — the 68000 reset convention — and
needs to know nothing else about it. See §9.

---

## 3. Interrupt architecture

**The MC68901 is the system interrupt controller**, as on real 68k boards. It
drives a single IPL line and supplies its own vector during the acknowledge
cycle. Peripheral interrupt outputs are wired to its GPIP pins.

This is also forced by the emulator: QEMU's m68k CPU accepts a level and vector
from exactly one source, so a vectored controller and an autovector controller
cannot coexist. There is no autovector controller on this board.

| MFP pin | Channel | Source | Also |
|---|---|---|---|
| GPIP5 | 7 | NS16550A UART | |
| GPIP4 | 6 | ATA | **TAI** — timer A event input |
| GPIP3 | 3 | LAN91C111 | **TBI** — timer B event input |
| GPIP2 | 2 | M48T59 | alarm and watchdog |
| GPIP1 | 1 | Intel 8042 | keyboard output buffer full |

Two GPIP pins doubling as timer event inputs is a property of the real chip, and
is used deliberately: timer A can count disk interrupts directly.

Vector = `(VR & 0xF0) | channel`. Channel 15 is highest priority, 0 lowest.
Full channel map and the rules that matter are in the programmer's guide.

---

## 4. The MC68901 model

QEMU had no MFP, so one was written. It implements:

- **All 24 registers** at their datasheet offsets.
- **16-channel vectored interrupt controller** — IERA/B, IPRA/B, ISRA/B,
  IMRA/B, correct priority order, vector generation, in-service inhibition of
  same-or-lower priority channels, and both automatic and software
  end-of-interrupt modes.
- **Timers A–D** in delay mode, all seven prescaler ratios, reload from the data
  registers, live readable down-counters.
- **Timers A and B in event-count mode**, counting active edges on TAI/TBI.
- **GPIP** with DDR direction control and AER edge selection.
- **USART** transmit and receive against a chardev, with all four USART
  interrupt channels.

Not modelled, and logged when touched: pulse-width measurement modes (timer
control values 9–15, treated as the equivalent delay mode); the USART's
synchronous modes, sync character register, break generation and parity.

---

## 5. Emulator changes

Three new files and edits to eight existing ones. Details and rationale in
[`qemu-patch/README.md`](qemu-patch/README.md).

| Change | Why |
|---|---|
| `hw/m68k/sage040.c` | the machine |
| `hw/misc/mc68901.c` + header | the MFP |
| Kconfig / meson entries | `CONFIG_SAGE040`, `CONFIG_MC68901`, and `select M48T59` |
| `hw/display/sm501.c`: guard the PCI variant with `#ifdef CONFIG_PCI` | upstream compiles it unconditionally, so a sysbus-only board fails to link. This board has no PCI bus and does not carry one — `CONFIG_PCI` is confirmed unset in the build. |
| `target/m68k`: optional `m68k_set_iack_handler()` | a vectored controller needs the interrupt-acknowledge cycle upstream documents as absent. Without it the MFP cannot clear the acknowledged channel, and every interrupt repeats forever. ~15 lines. |

The **M48T59** clock needed no new code at all: upstream already has a sysbus
variant, so the machine instantiates `sysbus-m48t59`, sets `base-year` to
2000 and maps it. That, rather than familiarity, is why the clock is not an
MC146818 — see §1.

---

## 6. Verification

Every device has a bare-metal test that exercises the real hardware path.
`make run` in `tests/`: **12 programs, all passing.** The kernel adds a
twelfth, `kernel/fstest.sh`, which drives a console session and then checks
the result with the host's own `mdir`, `mtype` and `fsck.fat` — 31 checks,
including loading and running a program from the disk, the tick running,
and a program drawing through `/dev/fb0`.

| Test | Checks | What it proves |
|---|---|---|
| `t1-cpu` | 7/7 | FPU against **exact IEEE-754 bit patterns** for pi, 1/3, `fsqrt(2)`; `mulu.l` 32×32→64 |
| `t2-uart` | 7/7 | Divisor latch behind DLAB; **local loopback** of six byte patterns through the real datapath |
| `t3-ata` | 7/7 | `IDENTIFY`, capacity **16384 sectors = 8 MiB matching the image**, 512-byte write and read-back compared byte for byte |
| `t4-net` | 11/11 | **Real ARP round trip** — reply from the slirp gateway at 10.0.2.2 |
| `t5-mmu` | 10/10 | Real three-level page tables, one page remapped to a different frame, verified in both directions |
| `t6-irq` | 11/11 | UART IRQ → GPIP5 → MFP channel 7 → **vector 0x47** → handler → `RTE` |
| `t7-mfp-irq` | 17/17 | Priority, vector generation, IER gating, IPR/ISR semantics, masking, **software EOI and in-service inhibition** |
| `t8-mfp-timers` | 10/10 | All four timers; **prescaler ratio measured at exactly 50** for /4 vs /200; event-count mode counting **real ATA interrupts** |
| `t9-mfp-usart` | 11/11 | Real transmit (verified against the output file) and real receive |
| `t10-sm501` | 7/7 | Device ID `0x050100A0`, 16 MiB non-aliasing, **640×480 framebuffer filled and verified** |
| `t12-kbd` | 9/9 | Self test, the command byte, and **which scancode set arrives** — `a` as `0x1E` and not `0x1C`, a release as `0x9E` and no `0xF0` prefix. Keys injected through QEMU's monitor |
| `t11-rtc` | 6/6 | NVRAM is real memory and does not alias onto the clock; every time field is in BCD range; the **oscillator advances**; a written date reads back; **30 February rolls into 1 March**, which is what the part does and why the driver validates first |

---

## 7. Status

| Stage | State |
|-------|-------|
| Machine, toolchain, boot | ✅ done |
| Boot ROM loading `KERNEL.ROM` from a FAT16 filesystem | ✅ done — `bootrom/` |
| Console, C runtime, exception vectors, interrupt dispatch | ✅ done |
| Disk, ethernet, video, MMU, timers | ✅ hardware proven by tests |
| Disk format (§9) — real MS-DOS, host read/write | ✅ done |
| Kernel (§10) — VFS, device model, drivers, FAT16 read/write, shell | ✅ done — `kernel/` |
| System calls — Linux/m68k convention, Linux numbers and errnos | ✅ done, and programs use them |
| Clock — M48T59, `time()`/`stime()`, file timestamps | ✅ done |
| Programs (§10) — ELF loader, `spawn`, argv, exit status | ✅ done — `user/` |
| System tick — MC68901 timer D, HZ=100, `nanosleep`, `times` | ✅ done |
| Framebuffer — `/dev/fb0`, point/line/rect/clear/flip, double buffered | ✅ done |
| Text console (§10) — `/dev/fbcon`, 80×30, IBM PC 8×16 font | ✅ done |
| Terminal (§10) — `tty.c`, many sources and sinks | ✅ done |
| Keyboard — Intel 8042, scancode set 1, `/dev/kbd0` | ✅ done |
| Ethernet driver — `struct netdev`, registered as `eth0` | ✅ written, only the probe is exercised |
| Shell — Linux-named commands, redirection, runs programs | ✅ done |
| Preemption, more than one program, user mode, virtual memory | unblocked — ordinary OS work now |
| TCP/IP (§8) | not started |

**Every hardware dependency is satisfied.** What remains is operating system,
not emulator.

---

## 8. TCP/IP stack

Not yet chosen. The hardware is proven — `t4-net` does a real ARP round trip
against the LAN91C111 — so this is a software decision.

### Leading candidate: lwIP

BSD-licensed and designed to be dropped onto bare metal. Its `NO_SYS=1` raw
mode needs no threads, no scheduler and no sockets layer — a main loop that
polls the NIC and calls `sys_check_timeouts()` is enough. That matters here,
because it does not require an operating system to exist first.

Three things make it fit this machine specifically:

- **Big-endian is the easy case.** Network byte order *is* big-endian, so
  `htons`/`ntohl` compile to nothing and a whole category of porting bug does
  not arise. (Note the contrast with this board's own devices: the ATA data
  register and every SM501 register are little-endian. Those are device
  quirks, not stack concerns.)
- **The 68040 does unaligned accesses in hardware.** Protocol headers are full
  of fields at awkward offsets; on ARM or MIPS that means packed-struct
  gymnastics or byte-at-a-time accessors. Here it is a cycle penalty and
  nothing more.
- **Two of the three port pieces already exist.** lwIP's port layer is a
  `cc.h` (types, packing, byte order), a `sys_now()` returning milliseconds,
  and one netif driver. MFP timer D already provides a 10 ms tick and a
  counter, and `tests/t4-net.c` already drives allocate / write-FIFO /
  enqueue and the receive path.

Expected cost: a netif driver of roughly 250 lines plus configuration.
Footprint lands around 30–40 KB — large beside the current test binaries,
irrelevant in 4 MB.

Known friction: the build is `-nostdinc`, and lwIP wants `string.h`. Some of
that is owed regardless — GCC emits calls to `memcpy` and `memset` on its own
for struct copies even in freestanding mode. `PACK_STRUCT` needs GCC's
`packed` attribute, which 15.2.0 has. Polled versus interrupt-driven receive
is a real choice: `NO_SYS` assumes polling, but the NIC is on MFP channel 3
if interrupt-driven is wanted.

### Alternatives, and when each would win

| Option | When it wins | Cost |
|---|---|---|
| **uIP** | Minimal footprint — ~5 KB, a few hundred bytes of RAM | One TCP segment in flight, so throughput is poor. Choosing constraint for its own sake on a 4 MB machine |
| **KA9Q NOS** | Period- and temperament-correct; it is what actually ran on 68k amateur gear | Expects to *be* the OS — own process model and scheduler. More work than lwIP, not less |
| **4.4BSD-Lite networking** | Historically authentic for a workstation of this vintage | mbufs, `splnet()`, deep entanglement with a BSD kernel that does not exist here. This is the "port NetBSD instead" path arriving by another route |
| **Write it** | ARP, ICMP echo and UDP are a few hundred lines, and `t4-net.c` has already started | TCP is where it stops being educational: retransmission, windowing, congestion control, and the state machine's edge cases |

### What was actually done, and why it differs

The easy layers were written by hand, as recommended — and then TCP was
too, which the recommendation above did not expect. The reason the advice
changed is that it was written when nothing existed: by the time TCP was
due, ARP, IPv4, ICMP and UDP were here, documented, and wired into the
device model and the shell. **lwIP is not a TCP.** It is a whole stack
with its own ARP, its own IP and its own idea of what an interface is, so
adopting it meant discarding all of that rather than slotting a layer on
top.

`net/socket.c` is what keeps the decision reversible. A program calls
`socket()`, `connect()` and `read()`; which implementation answers is
not its business, so lwIP can still replace what is underneath without a
program changing.

### What the TCP does not do

Each of these is a decision, written down so that it is a to-do rather
than a surprise. They are roughly in the order they would be worth
doing.

- **A random initial sequence number.** The ISN comes from the tick,
  which is guessable. On a LAN that is theoretical; on the open
  internet it lets an off-path attacker inject data into a connection.
  This is the only one on the list that is a security bug rather than a
  performance limit, and it should be fixed first. It needs a source of
  randomness the machine does not yet have — the obvious one is to hash
  the clock, the MAC and a counter, which is weak but enormously better
  than a multiplication.
- **Out-of-order reassembly.** A segment arriving ahead of a gap is
  dropped and the sender retransmits it. That is legal, and it costs
  throughput rather than correctness — but on any path that loses
  packets it turns one loss into a stall for a whole round trip. A
  reassembly queue is the single largest piece of TCP left undone.
- **Congestion control.** No slow start, no congestion window, no fast
  retransmit or recovery. The send window is whatever the peer
  advertised. A machine that only talks to its own LAN is not where the
  internet's congestion is decided, but anything going through a real
  path should not be sending a full window into a link it has not
  measured.
- **Round trip time estimation.** The retransmission timeout starts at
  500 ms and doubles, rather than being derived from measured RTT the
  way RFC 6298 describes. On a fast LAN that is far too slow to recover
  from a single loss; on a slow path it is too eager.
- **Window scaling, SACK and timestamps.** All are options and all are
  negotiated, so a peer that offers them works perfectly well with a
  stack that declines. Window scaling is what a transfer needs to go
  faster than about 64 KB in flight; SACK is what makes recovery from
  multiple losses in one window cheap. Neither matters until the two
  above are done.
- **Delayed and duplicate ACK handling.** Every segment is acknowledged
  immediately, which doubles the packet count on a bulk transfer.
- **Keepalives, and a real TIME_WAIT.** TIME_WAIT is ten seconds rather
  than twice the maximum segment lifetime, which is safe on a LAN where
  a segment cannot survive that long and is not on a long-haul path.

**Unverified:** whether a usable m68k reference port exists to crib a `cc.h`
from. ColdFire is 68k-family and was a common lwIP target under uClinux, so
the ABI and toolchain story should be well-trodden — but that is recollection,
not something checked, and it is worth ten minutes before counting on it.

---

## 9. Filesystem

**Decided and partly built: a real MS-DOS disk.** Not a FAT-like format of our
own, but a genuine partitioned FAT16 volume that the host reads and writes
with ordinary tools — `mtools`, `mount -t vfat`, `fdisk`, `fsck.fat`.

### Why

Host interoperability is the whole point. Putting a kernel on the disk is
`mcopy kernel.rom ::/`, not `dd` at a magic offset, and anything else on the
machine's disk can be inspected or edited from Linux without the guest
running. Every alternative means writing a host-side tool before a single
file can be placed.

It also has genuine 68k heritage — the Atari ST's GEMDOS filesystem is FAT
with quirks — and it is small: read-only FAT16 with 8.3 names came to about
200 lines.

FAT16 rather than 12 or 32. On a 99 MB partition with 2 KB clusters that is
50,579 data clusters, comfortably inside FAT16's range, with a 200-sector FAT.
FAT12 would mean unpacking 12-bit entries that straddle byte boundaries for no
benefit; FAT32 adds FSINFO and a cluster-chained root directory for capacity
that is not needed.

### Layout

```
LBA 0          MBR partition table
LBA 64         optional raw image, in the boot gap
LBA 2048       partition 1, type 0x06, FAT16, volume SAGE040
```

Sector 0 belongs to the partition table, so the kernel cannot live there. The
boot ROM reads the partition table, mounts the filesystem, finds
**`KERNEL.ROM`** in the root directory, follows its cluster chain to address 0
and jumps to it via the image's own 68000 reset vectors.

If there is no filesystem, or no `KERNEL.ROM` in it, it falls back to a raw
image at LBA 64 — the gap between the partition table and the first partition,
nearly a megabyte — so a disk with no filesystem still boots. All three paths
are exercised: file found, file missing with a raw image present, and neither.

The disk is built with the host's own tools and needs no root:

```
sfdisk        write the MBR
mkfs.fat -F 16 --offset 2048
mcopy -i hd.img@@1048576 kernel.rom ::/KERNEL.ROM
```

### What is implemented

Twice over, because the two have different jobs.

**In the boot ROM**, read-only and only as much as finding one file requires:
BPB parsing, the FAT16 cluster chain with a one-sector FAT cache, and a root
directory scan that skips deleted entries, long-name fragments, the volume
label and subdirectories.

**In the kernel** (`kernel/fs/fat16.c`), read *and* write over a real block
layer (`kernel/drivers/ata.c`): open, read, write, seek, create, truncate, append, delete,
rename, stat and a directory walk. Free-cluster allocation uses a rolling
hint so a sequential write walks the table once instead of restarting from
cluster 2 on every extension, and every FAT update is written to **both**
copies of the table.

`fsck.fat` reports the result clean after the kernel has written to it, and
`kernel/fstest.sh` checks exactly that: a file the kernel wrote comes back
byte for byte through `mtype`, a file the host wrote is what the kernel
printed, and `fsck.fat` finds nothing afterwards. A filesystem only the
kernel can read would prove nothing.

### What is not, yet

- **Subdirectories and long names.** The kernel looks only in the root and
  only at 8.3 names. Long-name entries the host wrote are skipped on a scan
  rather than misread, so a file created with one is still visible by its
  short name and is not damaged. Both are period-correct limitations.
- **Timestamps before the clock is set.** Stamps come from the M48T59, which
  reads the host's clock under emulation and a dead battery's idea of the
  time on hardware. If it does not answer, files get a fixed date — wrong
  but constant, which reads as obviously synthetic.
- **FAT12 and FAT32.** Refused at mount rather than misread as FAT16.
- **Crash consistency.** Writes go out as they are made, with no journal and
  no clean-shutdown flag. Pulling the plug mid-write leaves what MS-DOS would
  have left: lost clusters that `fsck.fat` can reclaim.

### The byte-order trap, for whoever writes the kernel side

**FAT is little-endian in every field and this machine is big-endian.** Every
BPB field, every FAT entry, every directory entry's cluster number and size
goes through `le16()`/`le32()`. Sector *data*, by contrast, is a byte stream
and needs no swapping at all.

That distinction already caused one bug: `t3-ata` swapped sector bytes in both
directions, which is self-consistent and passed its own round-trip test while
writing a byte-swapped image to the media. It surfaced only when the boot ROM
first tried to load something the host had written. Test against images the
host made and can still read afterwards; `fsck.fat` and `mdir` are the
verification that a round trip cannot give you.

---

## 10. The kernel

`kernel/`, loaded from the disk by the boot ROM as `KERNEL.ROM`. It runs in
supervisor mode from its first instruction and never leaves it.

### The shape

```
          shell.c  edit.c        programs that happen to be linked in
 ------------------------------  trap #0
              syscall.c          open read write lseek stat getdents ...
               vfs.c             paths, mounts, the descriptor table
      +-----------+-----------+
   fs/fat16.c           dev.c    filesystem types, device registries
      |                   |
 struct blockdev     chardev / netdev / rtcdev / timerdev / fbdev
      |                   |
 drivers/ata.c       drivers/ns16550.c  m48t59.c  mfp.c  sm501.c
                     drivers/smc91c111.c
```

Three properties are worth stating because they are what the layering is
for, and each is checkable rather than aspirational:

- **The shell reaches the filesystem, the disk and the terminal only
  through `trap #0`.** Every command in it is system calls and nothing
  else, and `kernel/layercheck.sh` fails the build if that stops being
  true. It was asserted here while it was false — `cmd_console` had
  grown a direct call into `tty.c` — which is the argument for checking
  the rule rather than restating it.
- **A driver probes before it pokes.** An address with no device behind
  it bus-errors rather than reading zeroes, so every driver asks with
  `io_probe8`/`16`/`32` first and `main.c` reports what is absent. A
  kernel on an emulator built before one of its devices existed says so
  instead of panicking.
- **The line editor is above the boundary too.** `edit.c` clears
  `ICANON` and `ECHO` with `TCSETS` and does the editing, the history
  and the searching itself, which is where bash keeps that work. It
  includes `syscall.h` and nothing else and would compile unchanged as
  an ordinary program.
- **The filesystem talks to a `struct blockdev`** and has no idea an ATA
  taskfile answers. A SCSI controller or a RAM disk is a new file in
  `drivers/` and one more line in `main.c`.
- **`main.c` is the only file that names a chip.** Deliberate: this is a
  board with parts soldered to it, not a bus that can be enumerated, so
  something has to know what is fitted — and exactly one thing does.

### System calls

The convention is Linux/m68k's, unchanged: `d0` holds the call number,
`d1`–`d5` the arguments, and `d0` comes back with the result or a negated
errno. That is not an imitation — Linux picked the obvious convention for
this architecture and there is nothing to improve on. The numbers are
Linux's i386 numbers, because `__NR_write` being 4 is a fact a lot of
people carry around, and the errnos are Linux's by name and value.

`uapi.h` holds what crosses the boundary and nothing else, the same split
Linux makes under the same name: a program gets `O_CREAT` and
`struct stat`, never `struct fs_type` or the descriptor table.

`kmain()` makes a call through the gate at startup and checks that an
unknown number comes back `-ENOSYS`, so the path is known good where it is
installed rather than where something first depends on it.

**Still on the wrong side of the line:** `syscall_dispatch()` takes pointer
arguments at face value. Correct while every caller shares the kernel's
address space, and the function that will have to validate and copy them
when that stops being true. The MMU is off, so there is no address space to
separate yet.

### The tick

`drivers/mfp.c` is both the interrupt controller and the system timer. The
MC68901 drives one IPL line and supplies its own vector, so its sixteen
channels arrive at sixteen consecutive vectors and one stub serves all of
them — it recovers the channel from the format/vector word the 68040
pushed. A driver asks for a channel with `mfp_request_irq()`; nothing
above that knows the chip exists.

Timer D at /200 runs at 12288 Hz and a reload of 123 gives **99.9 Hz**.
`HZ` is 100 — what Linux used for most of its life, and a 10 ms tick that
makes a 50 fps frame exactly two of them.

The reload is computed rather than written down, and clamped: a value
under 8 is refused outright. A tick shorter than its own handler starves
the foreground completely, and this machine already walked into that once
with a timer at 13 µs. Sleeping uses `STOP`, so an idle program costs the
host nothing, and `timer_sleep_ticks()` returns `-ENODEV` rather than
waiting forever when no timer is running.

Interrupts are enabled **last** in startup, after every driver is up: a
fault before that point is reported by a handler with the console to
itself, and an interrupt arriving mid-initialisation would be a much
harder thing to understand.

### The framebuffer

`/dev/fb0`, drawn with ioctls — `FBIO_POINT`, `FBIO_LINE`, `FBIO_RECT`,
`FBIO_CLEAR`, `FBIO_FLIP`, `FBIO_PALETTE`, `FBIO_GETINFO`.

Through ioctl rather than through system calls of its own, because a
framebuffer is a device and the device model already carries it. A dozen
graphics calls in the system call table would tie the kernel's ABI to one
kind of hardware. Linux controls its framebuffer the same way, though
Linux then expects a program to `mmap` the memory and draw for itself,
which needs an MMU that is off here.

**Only `point()` is required of a driver.** `fb.c` builds clear, line and
rect from it, so a new display works as soon as it can set one pixel, and
gets faster as its driver learns to do more. `sm501.c` implements clear
and filled rect with the 2D engine — at a period-correct clock the CPU
cannot clear 640×480 and hold a frame rate, 37 fps against the engine's
50, while a dozen short lines cost nothing either way.

Double buffered. `FBIO_FLIP` is one register write, so the change lands
between frames rather than halfway down one.

### The terminal, and the text console

`/dev/console` is `tty.c`: a line discipline plus a list of input sources
and a list of output sinks. A UART is one place characters can come from
and go to; the screen is another.

`/dev/fbcon` is 80 columns by 30 rows of the IBM PC 8×16 font, green on
black — 640×480 over the character cell, the geometry a VGA text mode had
for the same reason. It is an output sink and nothing else.

**Output goes to every enabled sink at once and input is taken from every
source**, so the shell is on the screen and on the serial line together
rather than on one of them. `console NAME off` silences a sink; the last
one cannot be silenced.

That is a constraint rather than a preference. The test harnesses drive
this machine over the serial line with `-display none`, and QEMU delivers
no keyboard input at all without a display — so an exclusive console
would break every test in the tree, and would also lose the serial log at
exactly the moment the display path is what is broken.

The split forced one thing out of the UART driver: **echo belongs to the
terminal, not to the chip the character arrived on.** With the line
discipline inside the driver, output on the screen meant typing blind.

Scrolling is a single `copy()` — the blitter moves 29 rows in one
operation. Without one, `copy` is left null and the console redraws from
its own character buffer, which is why `fbdev` has the operation at all.

The font is the actual VGA ROM font, extracted rather than redrawn; see
`kernel/font8x16.c` for its provenance and licence.

### Devices

Six classes, each with one interface: `chardev` (a byte stream),
`blockdev` (sectors), `netdev` (packets), `rtcdev` (seconds since 1970),
`timerdev` (a periodic interrupt), `fbdev` (a display).
Drivers register during startup and stay registered until the power goes
off — no hotplug, no refcounting, nothing to unregister, because with a
handful of soldered parts that would be machinery in search of a problem.

Path resolution has two fixed mount points: `/dev` is the device registry,
everything else is the mounted volume. That is honest about a filesystem
with one directory and does not change the calls above it when that stops
being true.

### The terminal

`drivers/ns16550.c` is a terminal, not just a UART. `read()` returns one
whole line, echoed as typed, with backspace and ctrl-U working and ctrl-D
returning 0 for end of input — canonical mode, in the driver for the same
reason it lives in the tty layer on a real system: otherwise every program
that reads a line implements it again, slightly differently. ONLCR and
ICRNL are applied here too, so a file written through the same `write()`
gets the bare newline it should have.

Polled in both directions, on purpose: it works before interrupts are set
up, works inside a panic, and cannot deadlock against the code reporting
the fault.

### Sizing memory, and surviving it

Nothing on this machine reports how much RAM is fitted, so the kernel writes
to each megabyte boundary until one does not answer. The first address past
the end raises a **bus error** rather than reading back wrong — QEMU faults
there exactly as hardware with no card in the slot would — so `memprobe.s`
installs its own bus error handler, throws the frame away and returns as if
the test had failed. The traditional 68k ROM trick, and still the only way
to do it here.

It is deliberately narrow. The recovery abandons the exception frame and
restores the stack pointer by hand, which is safe only because the routine
touches no callee-saved register and holds no state worth unwinding. It is
not a general fault handler and should not grow into one.

### Faults

Every vector except reset lands on one handler. The 68040 pushes a
format/vector word on every exception, so the handler identifies itself from
its own frame instead of needing 255 stubs, and reports the vector, its
name, the PC, the SR and all fifteen registers before halting.

Nothing is recoverable yet, so it stops — a silent hang is the one outcome
worth ruling out. That report found the first real bug in the kernel: the
memory probe walking off the end of RAM, faulting address in `a0`.

---

## 11. Open items

1. **Lift the remaining test code into drivers.** `t3` became
   `kernel/drivers/ata.c`, `t4` became `drivers/smc91c111.c`, `t7`/`t8`
   became `drivers/mfp.c` and `t10` became `drivers/sm501.c`. What is left
   in tests and not in a driver is the MFP's USART (`t9`) and the MMU
   (`t5`) — the second of which is not a driver at all but the thing
   processes will need.
2. **Interrupt-driven input.** Both the serial port and the keyboard are
   polled, and both have an interrupt line already wired to the MFP —
   channel 7 and channel 1. They should fill one ring buffer that
   `tty.c` drains, which would remove the last polling loop in the
   system and let a waiting `read()` use `STOP` the way `nanosleep`
   already does.
3. **A scheduler.** The tick exists and drives `nanosleep`; what it does not
   yet do is preempt anything, because there is only one thing to run.

   **The memory half is now done.** Every program runs in user mode in
   an address space of its own, with its own page tables, its own
   physical pages and its own kernel stack. Two programs would not
   collide, and `kernel/vmtest.sh` demonstrates that neither can reach
   the kernel, the devices, or anything it was not given.

   **What tasks still need, in the order they will be wanted:**

   - **A run queue**, and a `struct task` that `struct job` becomes.
     The job table already carries state, a saved context, an address
     space and a kernel stack; what it lacks is a notion of
     runnable-versus-blocked and something to pick the next one.
   - **A full context switch.** `exec_stop`/`exec_resume` save only the
     callee-saved registers, which is enough at a C call boundary and
     not enough anywhere else. Preemption means saving every register
     and the PC out of the exception frame.
   - **Wait queues.** A blocked task has to be somewhere. Every
     polling loop in the system -- `tty.c`'s `next_char()` above all --
     becomes a sleep on a queue that the driver's interrupt wakes,
     which is also what finally removes the spin loops.
   - **Semaphores**, which are the more basic of the two: a counting
     semaphore is what a wait queue is made of, and the binary case is
     what a driver uses to say "the transfer you asked for has
     finished". Worth having before mutexes rather than after, because
     a mutex is a semaphore with an owner and the owner is the part
     that only matters once priorities do.
   - **Mutexes**, once more than one task can be inside the kernel at
     once. The VFS, the block layer and the terminal all hold state
     that is currently safe only because nothing else can run.
     Interrupt handlers will need the non-blocking kind.
   - **Real signals.** What exists now is one-directional: the kernel
     does something *to* a job. Tasks need delivery to a handler,
     blocking and pending sets, and `kill()` between tasks -- at which
     point `job_signal_fg` becomes the terminal's special case of a
     general mechanism rather than the whole of it.

4. **An environment, and a PATH.** The shell looks a command up by name
   in the one directory this volume has, because that is all there is;
   a `PATH` implies an environment to keep it in, and an environment
   implies that programs inherit one.

   The mechanism is already half built and in the right place.
   `setup_stack()` copies the argument vector into the new address
   space -- strings first, then an array of pointers to them -- because
   the shell's own memory is not reachable from a program any more. An
   environment is the same operation a second time, and it lands next
   to the first: the conventional Unix layout puts `envp` directly
   above `argv` on the stack, with `crt0.s` taking a third argument and
   `main(argc, argv, envp)` behind it. What has to be decided is where
   the shell keeps its own copy, since it has no heap, and how much of
   a fixed budget a program's environment may occupy.

   `PATH` itself then needs the filesystem to have more than one
   directory, which FAT16 has and this kernel does not yet use -- so
   the two are worth doing together.

5. **The network tools should be programs, not builtins.** `ifconfig`,
   `ping`, `arp` and `arping` are commands inside the shell, which was
   right when there was no way for a program to reach the network and is
   not now that there are sockets. `netstat` does not exist at all and
   wants the same information the shell's `jobs` gets -- a listing of
   what the kernel is holding, which means `netctl` growing a way to
   walk the socket and connection tables rather than just the interface.

   Moving them out is not cosmetic. A builtin runs in the kernel with
   the kernel's privileges; a program runs unprivileged in an address
   space of its own and can only do what the system call interface
   allows. Anything that can be a program should be one, and the ones
   that cannot are the argument for a system call that is missing.

6. **Split the system software from the applications.** `user/` holds
   the shell, ping, ifconfig and shutdown alongside cube and fbtest,
   which puts a graphics demo and the program that stops the machine in
   the same place. They are not the same kind of thing: one set is part
   of the system and expected to be there, the other is what somebody
   chose to run on it.

   The split wants the same subdirectories `PATH` and `/etc` want --
   `/bin` for the system's own programs and somewhere else for
   everything else -- and it is the reason PATH is worth having at all,
   since with one directory there is nothing for a search order to
   choose between.

7. **Configuration from files, and a startup script.** Everything the
   machine knows about itself is currently either compiled in or typed
   at the prompt: the IP address is `ifconfig` every boot, the console
   layout is `console` every boot, and none of it survives a restart.
   The answer is the one Unix settled on -- a directory of small text
   files that the system reads at startup -- and it arrives in three
   pieces that depend on each other in this order:

   - **Subdirectories in the filesystem.** `/etc` has to be able to
     exist before anything can live in it. FAT16 has directories and
     `fs/fat16.c` looks only in the root, so this is the same
     prerequisite `PATH` has.
   - **Shell scripting.** A startup file is only worth having if the
     shell can run one: reading a file as a sequence of commands,
     comments, and enough conditional to let a script cope with a
     machine where something is absent. `run_command()` is already
     separated from the prompt loop for exactly this reason -- `fg` on
     a queued job needed to run a command line with no prompt
     involved, and a script is the same need repeated.
   - **The files themselves.** `/etc/rc.local` run at startup once
     there is a shell that can run it, and `/etc/network` or similar
     read by the network code. Worth resisting the temptation to
     invent a parser per subsystem: one "key value per line" reader,
     used by everything, is the difference between configuration and a
     collection of formats.

   Note the ordering against DHCP. A machine that gets its address
   from the network needs no address in a file -- but it does need to
   be told whether to ask, and that is itself configuration.

   **Two thirds of the context-switch plumbing is already there, and it
   was put there by ctrl-Z rather than planned.** `job.c` holds a table of jobs with
   states and pending signals; `exec_stop()` and `exec_resume()` in
   `execasm.s` are a context switch — each saves the callee-saved
   registers and the stack pointer and jumps to where the other left
   off. `fg` on a stopped job uses exactly that. What a scheduler adds
   is a run queue, a decision at the tick, and a **full** context save:
   today a job can only be stopped at a system call boundary, because
   resuming from an arbitrary instruction means saving every register
   and the program counter out of the exception frame. ctrl-C already
   does unwind from the tick, so the interrupt-side half of that is
   proven.

   Two things are refused today for want of it, and both are enforced
   rather than documented. `&` and `bg` record a job and say plainly
   that nothing can run in the background yet. And only one program can
   be loaded at a time — a stopped job holds the image area at 1 MB, so
   `exec.c` refuses a second spawn instead of loading over it. That one
   goes away with the MMU rather than the scheduler.
4. **An interrupt-driven console.** The UART's IRQ already reaches MFP channel
   7 and `t6` proves the whole path. `kgetc()` takes from a ring buffer instead
   of the line status register and nothing above it moves.
5. **Pick a TCP/IP stack** (§8). lwIP is the recommendation; nothing blocks it
   now that the NIC is proven.
6. **Use the ethernet driver.** `drivers/smc91c111.c` implements
   `struct netdev` — up, down, send, receive — but only its probe runs at
   startup. Nothing transmits or receives a packet until there is a stack
   above it (§8), so the send and receive paths are written and unexercised.
   A test that does an ARP round trip through the driver, as `t4-net` does
   from bare metal, is the cheap way to close that.
7. **Use the NVRAM.** 8176 bytes that survive a power cycle, with nothing in
   them. Boot settings are the obvious tenant, and it wants a checksum and a
   small structure rather than raw offsets.
8. **Consider upstreaming** the `sm501.c` build fix and the IACK callback.
9. **Hardware.** Nothing in the design needs a bus that cannot be wired by hand:
   a 68040, an MFP, and six memory-mapped peripherals.

---

## 12. Next

1. **A scheduler tick** — timer D at ~10 ms driving a counter, then a scheduler.
   Everything it needs is tested.
2. **An interrupt-driven console** — `t6` has all the pieces; turning the polled
   `kernel/console.c` into a buffered interrupt-driven one is the natural first
   use of the interrupt path.
3. **Then processes.** Most of the way there already: programs load at their
   own address, run on their own stack, take arguments, return an exit
   status, and there is now a tick to preempt them with. What is left is
   entering **user mode** with an `RTE` instead of a `jsr`, validating the
   pointers that then arrive across the gate, and a run queue. The MMU is
   available when isolation is wanted rather than merely privilege.
