# Sage040 — design

A 68040 workstation built entirely from **real, datasheet-backed silicon**. No
virtio, no paravirtual devices, no PCI. Every part below is a chip you could
buy and solder, chosen so the design could plausibly be built in hardware.

Implemented as a custom QEMU machine, `sage040`.

**[`os.md`](os.md)** describes SuckOS, the system that runs on it; this file
is the machine, and the decisions behind it.

- **[`programmer-guide.md`](programmer-guide.md)** — how to write code for it
- [`qemu-patch/`](qemu-patch/) — the emulator, reproducible from pristine source
- [`tests/`](tests/) — twelve device tests, `make run`
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
needs to know nothing else about it. See §8.

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
`make run` in `tests/` runs all twelve. The system's own suites, which boot
the machine and drive it over its serial line, are listed in `os.md`;
`make test` runs everything.

Verifying the guest's writes with the *host's* tools rather than by reading
them back with the same code that wrote them is deliberate: `t3-ata` is the
standing reminder that a round trip cannot catch a byte-order error,
because both directions swap.

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

## 7. TCP/IP stack

**Settled: written out, not imported.** The stack is in `kernel/net/` —
ARP, IP, ICMP, UDP, DHCP, TCP and a socket layer — and it works against
real hosts on a real LAN. `os.md` describes what it does; this section is
why it is not lwIP, which is the obvious answer for a machine this size.

### The case for lwIP, and why it does not apply

For a machine with a few MB of RAM, lwIP is the obvious answer: 40 KB of
code, a `netif` driver of roughly 250 lines, no dynamic allocation
required, a raw API that avoids threads entirely, and a BSD-socket
compatibility layer on top. It is the standard choice for exactly this
size of system.

That case holds **when the layers below TCP do not exist.** lwIP is not a
TCP: it is a whole stack, with its own ARP, its own IP and its own idea
of what an interface is. Once ARP, IP, ICMP and UDP are written and wired
into the device model, adopting it means *discarding* what works and
adapting to its device model, not slotting a layer in on top.

What keeps the decision reversible is the socket layer: a program calls
`socket()`, `connect()` and `read()`, and which implementation answers is
not its business. If lwIP is ever wanted, `net/socket.c` is the seam.

### Alternatives, and when each would win

| | When it wins |
|---|---|
| **lwIP** | If the stack below TCP did not already exist, or if IPv6, DNS and DHCP-with-options were all wanted at once |
| **uIP** | A far smaller machine — one segment in flight, no window worth the name |
| **Written out** | What is here: the lower layers exist, and TCP was the only missing piece |

### What the TCP does

RFC 793's state machine, both opens, an orderly close on both sides, and:

- **Out-of-order reassembly**, from a shared pool rather than per
  connection — a reassembly queue is only occupied during a loss, so
  giving every connection its own reserves memory for a situation that is
  rare on all of them at once
- **Congestion control**, RFC 5681: slow start, congestion avoidance,
  fast retransmit, fast recovery
- **RTT measurement and a computed RTO**, RFC 6298, with Karn's algorithm
- **Delayed acknowledgements**, and duplicate-ACK handling as the
  fast-retransmit trigger
- **Window scaling**, RFC 7323, shift 2. The buffers are 64 KB to send
  and 128 KB to receive, page-allocated when a connection is made, so an
  idle socket costs nothing and a busy one can keep more than 16 bits'
  worth of window in flight
- **Timestamps and PAWS**, RFC 7323. The RTT is measured from the echoed
  timestamp on every acknowledgement rather than once a round trip, and
  a segment whose timestamp is older than the last one seen is dropped
- **SACK**, RFC 2018: the receiver reports up to three blocks from its
  reassembly queue; the sender keeps a scoreboard of eight, resends the
  holes below the highest SACKed byte during recovery, and steps over
  SACKed data when a timeout sends it back to `snd_una`
- **Keepalives**, `SO_KEEPALIVE` with Linux's `TCP_KEEPIDLE`,
  `TCP_KEEPINTVL` and `TCP_KEEPCNT` (7200 s, 75 s, 9, as on Linux). A
  connection given up reports `ETIMEDOUT`
- **A real `TIME_WAIT`**: 60 seconds, Linux's figure for 2 MSL. A
  retransmitted FIN is acknowledged again and restarts it, and a reset
  does not cut it short (RFC 1337)
- **Initial sequence numbers that cannot be guessed**, RFC 6528, over
  `kernel/random.c` — a BLAKE2s pool fed by interrupt timing with
  ChaCha20 output, described in `os.md`

Every item on that list matters because the interface can be bridged onto
a real LAN. On QEMU's NAT none of it shows: without reassembly a single
lost packet stalls a transfer for a whole round trip, and an ISN of
`jiffies * 7919` is guessable by anyone who knows roughly when the
connection was made.

### What the TCP does not do

Each of these is a decision.

- **Path MTU discovery.** The MSS is 1400, which fits an ethernet frame
  with room for a tunnel's headers.
- **Nagle.** Small writes go out as they are made; `TCP_NODELAY` is
  always on. A human at the other end is not where the forty-byte-header
  problem is solved, and coalescing would make an interactive session
  worse.
- **Buffer sizes per socket.** `SO_SNDBUF` and `SO_RCVBUF` are accepted
  and ignored, and report the fixed sizes.

**How the options are tested** (`kernel/tcptest.sh`, 24 checks): both
ends over loopback, with two knobs in `netctl` that make the network
misbehave in known ways -- `NETCTL_TCPLOSS` drops every Nth outgoing
data segment, `NETCTL_TCPOPTS` stops new connections offering an
option. "SACK works" is not something a test can observe; "on the same
losses, the sender with SACK retransmitted less than the one without"
is. Because loopback has this stack at both ends, an option written and
read wrongly *in the same way* would pass there -- so `nettest.sh` also
captures the guest's frames against QEMU's NAT and decodes the SYN's
options on the host (`kernel/synopts.py`). Sending window scale as
option 30 on both sides passes all 24 loopback checks and fails that
one.

And above it, **a resolver** (task 18, `lib/resolv.c`): `/etc/hosts`,
`localhost`, then DNS over UDP to the servers in `/etc/resolv.conf` or
the one DHCP gave. `host`, `ping`, `fetch` and `ntpdate` take names.

## 8. Filesystem

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
layer (`kernel/drivers/ata.c`): open, read, write, seek, create, truncate,
append, delete, rename, stat and a directory walk — **subdirectories**, with
`mkdir`, `rmdir`, a per-task working directory, `chdir` and `getcwd` —
**long names**, VFAT's UTF-16 encoded from the UTF-8 a program uses — and a
`fsck` of its own, with the clean-unmount flag in the boot sector that says
when one is needed.

Two structural facts about FAT16 make that more than a loop change. **A
directory is one of two things**: the root is a fixed run of sectors that
cannot grow, and every other directory is an ordinary cluster chain. FAT32
abolished the distinction; FAT16 did not, so `struct dir` carries a cluster
number with 0 meaning the root. And **`.` and `..` are the only record of a
directory's parent** — a FAT directory entry says nothing about where it
lives — so `mkdir` must write both or the directory cannot be left. The
parent of a directory in the root is recorded as cluster 0.

Free-cluster allocation uses a rolling
hint so a sequential write walks the table once instead of restarting from
cluster 2 on every extension, and every FAT update is written to **both**
copies of the table.

`fsck.fat` reports the result clean after the kernel has written to it, and
`kernel/fstest.sh` checks exactly that: a file the kernel wrote comes back
byte for byte through `mtype`, a file the host wrote is what the kernel
printed, and `fsck.fat` finds nothing afterwards. A filesystem only the
kernel can read would prove nothing.

### What FAT cannot hold

- **Permissions, ownership, links and FIFOs.** There is nowhere to put any
  of them: `ls -l` shows a mode because `stat` synthesises one, and `link`,
  `symlink` and `mknod` answer `EPERM` as a Linux FAT mount does. Making
  the system genuinely multi-user is therefore a filesystem change as much
  as a kernel one (§9).
- **FAT12 and FAT32.** Refused at mount rather than misread as FAT16.
- **A journal.** Writes go out as they are made. Pulling the plug mid-write
  leaves what MS-DOS would have left — lost clusters and cross-links — and
  the volume is marked dirty, so the next boot checks it. `mount` sets that
  flag and only an orderly `halt`, `reboot` or `shutdown` clears it.

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


---

## 9. Not there yet

The system that runs on this machine is described in [`os.md`](os.md), and
what remains to be built is listed in [`progress.md`](progress.md). Two
items belong here rather than there, because they are decisions about the
machine:

- **Permissions and ownership need a filesystem that can hold them.** FAT
  cannot (§8), so a genuinely multi-user system means a second filesystem
  type under the VFS, not a change to `fat16.c`. The USERS are there now
  -- a task carries a real, effective and saved uid and gid, `/etc/passwd`
  names them, and ssh authenticates into them -- so what is missing is
  only the enforcement, and the enforcement is the filesystem's.
- **`dlopen` is the loader's, not the library's.** `ld.so` resolves what
  a program was linked against and stops. libffi is built and works, and
  Python's `ctypes` still cannot be built, because it opens libraries by
  name at run time. Adding it means the loader learning to map and
  relocate an object that nothing referenced at link time.
- **Thread-local storage needs three things at once**: `PT_TLS` handled
  in `ld.so`, a per-thread block allocated at clone, and
  `__m68k_read_tp` in the C library -- the 68040 has no thread pointer
  register, which is why the compiler emits a call rather than an
  instruction. Software that uses `__thread` as an optimisation falls
  back to a global without it.
- **Upstreaming.** The `sm501.c` build fix and the IACK callback are both
  plausibly of general use, which is why `qemu-patch/` is
  GPL-2.0-or-later rather than matching the rest of the tree.

Nothing is on a "deliberately not doing" list. Paging, shared libraries and
the remaining TCP options each sat on one once, justified by the machine
being small; it is not small — 64 MB of RAM and a 512 MB disk — so the
justification went with them.

**The machine builds its own programs.** binutils runs on it, the C
library and the linker script are on its disk, and gcc is the last piece
(`toolchain.md`, "The toolchain that runs ON the machine"). That was not
on a list of things to avoid either; it was simply a long way down a
chain that had to be built first.
