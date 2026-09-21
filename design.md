# Sage040 — design

A 68040 workstation built entirely from **real, datasheet-backed silicon**. No
virtio, no paravirtual devices, no PCI. Every part below is a chip you could
buy and solder, chosen so the design could plausibly be built in hardware.

Implemented as a custom QEMU machine, `sage040`.

- **[`programmer-guide.md`](programmer-guide.md)** — how to write code for it
- [`qemu-patch/`](qemu-patch/) — the emulator, reproducible from pristine source
- [`tests/`](tests/) — ten device tests, `make run`
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
`make run` in `tests/`: **11 programs, all passing.** The kernel adds a
twelfth, `kernel/fstest.sh`, which drives a console session and then checks
the result with the host's own `mdir`, `mtype` and `fsck.fat` — 21 checks,
including loading and running a program from the disk.

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
| System calls — Linux/m68k convention, Linux numbers and errnos | ✅ done, no user programs to use them yet |
| Clock — M48T59, `time()`/`stime()`, file timestamps | ✅ done |
| Programs (§10) — ELF loader, `spawn`, argv, exit status | ✅ done — `user/` |
| Ethernet driver — `struct netdev`, registered as `eth0` | ✅ written, only the probe is exercised |
| Timer + preemption, processes, virtual memory | unblocked — ordinary OS work now |
| TCP/IP (§8), framebuffer console | not started |

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

### Recommendation

lwIP in `NO_SYS` mode. Ping, then UDP echo, then TCP. Writing the easy layers
by hand first is worthwhile for its own sake; bring lwIP in at TCP.

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

**In the kernel** (`kernel/fs.c`), read *and* write over a real block layer
(`kernel/ata.c`): open, read, write, seek, create, truncate, append, delete,
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
- **Timestamps.** There is no real-time clock on this machine, so every stamp
  the kernel writes is a fixed date. A stamp that is wrong but constant is
  better than one that is wrong and varies: it is obviously synthetic. An
  MC146818 is the fix and is in the open items.
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

Three properties are worth stating because they are what the layering is
for, and each is checkable rather than aspirational:

- **`shell.c` includes `syscall.h` and nothing else from the kernel.** Not
  `vfs.h`, not `dev.h`, not `console.h`. It cannot reach a filesystem or a
  chip even by accident.
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

### Devices

Four classes, each with one interface: `chardev` (a byte stream),
`blockdev` (sectors), `netdev` (packets), `rtcdev` (seconds since 1970).
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

1. **A framebuffer device.** The most visible gap now that programs exist:
   `user/cube.c` includes the machine's hardware header and writes to the
   SM501 directly, because a display fits none of the classes in `dev.h`. It
   needs one — and then the cube stops reaching around the kernel, which is
   the last thing in `user/` that does.
2. **Lift the remaining test code into drivers.** `t3` became
   `kernel/drivers/ata.c` and `t4` became `drivers/smc91c111.c`. `t7`–`t11`
   are still proven working code living in tests; the MFP wants turning into
   a driver with a real interface.
3. **Pick a scheduler tick.** Timer C or D at /200 with a reload near 123 gives
   ~10 ms. Note the livelock bound documented in the programmer's guide: a tick
   faster than the handler starves the foreground.
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
   own address, run on their own stack, take arguments and return an exit
   status. What is left is entering **user mode** with an `RTE` instead of a
   `jsr`, validating the pointers that then arrive across the gate, and a
   scheduler to have more than one at a time. The MMU is available when
   isolation is wanted rather than merely privilege.
