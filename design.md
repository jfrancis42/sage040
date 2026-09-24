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
| CPU, FPU, MMU, caches | Motorola **MC68040** | On-chip FPU, a working paged MMU, and 4 KB each of instruction and data cache -- both enabled, with the page tables marked non-cachable because the table walker does not snoop the data cache (os.md, "Memory") |
| Timers, interrupts, 2nd serial | Motorola **MC68901 MFP** | The classic 68k companion chip; one part gives timers, a vectored interrupt controller, a parallel port and a USART |
| Console | National **NS16550A** | The most thoroughly documented UART ever made; a working console is ~20 lines |
| Disk | **ATA taskfile** (WD1003 lineage) | Eight registers, polled PIO, no DMA or descriptors — ~60 lines for read and write |
| Ethernet | SMSC **LAN91C111** | On-chip packet FIFO with **no descriptor rings in host memory**, which is what makes it far easier than a SONIC or LANCE. **Four pages, shared between transmit and receive** -- see below |
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

### The LAN91C111's four pages

The chip holds frames in an on-chip pool of **four pages**, and
transmit and receive draw on the same four. Two consequences follow
and neither is optional:

- **Received frames must be taken off the chip promptly.** A frame
  left there holds its page, and four held pages mean the chip can no
  longer send. `net_drain()` runs from the timer interrupt for this
  reason rather than waiting for a task to get round to it.
- **A transmit allocation is a standing REQUEST, not a question.**
  When no page is free the chip remembers the request and grants one
  as soon as a page is released, raising ALLOC then. A driver that
  gives up and asks again abandons that grant -- the new command
  clears the old request, and nothing ever releases the page it was
  given. Four abandonments and the chip is dead in both directions.
  `smc_send()` therefore takes a grant that is already waiting rather
  than issuing a second request.

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

**Decided and built: ext2, the real one.** Not an ext2-like format of our
own, but a genuine partitioned ext2 volume that the host reads, writes and
checks with e2fsprogs — `mke2fs`, `e2fsck`, `debugfs`, `dumpe2fs` — on the
plain image file.

### Why

The same reason the disk was a real MS-DOS disk before it was this: a
filesystem only this kernel can read proves nothing. Being able to hand the
image to somebody else's code is what makes a test a test. `e2fsck` is worth
more here than any assertion the driver could make about itself, because it
recomputes every link count, every bitmap and every directory's `.` and `..`
from the disk alone — so a driver that keeps a filesystem *it* is happy with
still fails.

What ext2 buys over FAT16, and why the machine's own disk moved:

- **Permissions and ownership.** A file has a mode, a uid and a gid. `ls -l`
  means something, and `/etc/passwd` describing users that files can belong
  to is no longer a fiction.
- **Names are bytes.** No 8.3, no aliases, no case folding, no upper-casing
  on the way in. `winchtest` is `winchtest` and not `WINCHTES`, and
  `Makefile` and `makefile` are two files.
- **An inode number that identifies a file for its whole life**, rather than
  a number derived from the directory slot it happens to sit in.
- **Hard links**, sparse files, and a free-space map that is a bitmap rather
  than a chain to be walked.
- **Timestamps to 2106** rather than 2038 — see below.

FAT16 has NOT been removed. `kernel/fs/fat16.c` is still built and still
registered, and the mount probes ext2 first and falls back to it, because a
disk from a machine that has never heard of this one is still a FAT disk.
What changed is which filesystem this machine keeps its own system on.

### Layout

```
LBA 0          MBR partition table
LBA 64         optional raw kernel image, in the boot gap
LBA 2048       partition 1, type 0x83, ext2, volume SAGE040
```

Made with 4 KB blocks and 256-byte inodes:

```bash
mke2fs -t ext2 -b 4096 -I 256 -O ^dir_index,^resize_inode -L SAGE040 \
       -E offset=1048576 hd.img
```

Each of those is load-bearing.

**4 KB blocks.** Twelve direct pointers reach 48 KB and one indirect block
reaches 4 MB, so the boot ROM needs no double indirection to load a kernel.
At 1 KB it would.

**256-byte inodes** leave room for the inode's "extra" word, which is what
carries a date past 2038. This kernel's `time_t` is an unsigned 32-bit count
and ext2's `i_mtime` is a signed one; a date after 2038-01-19 is stored as
the same 32 bits with the extra word's epoch bits set to 1, which is how
ext4 spells it and what makes Linux and e2fsprogs read back the date this
kernel meant.

**`^dir_index`.** A hashed directory is *readable* by a driver that knows
nothing about it — the interior nodes are shaped like empty entries on
purpose — but writing into one without maintaining the hash tree leaves an
index that no longer finds the names underneath it. The driver refuses to
mount a volume with the feature rather than quietly corrupt one.

**`^resize_inode`.** Nothing here resizes a volume, and the reserved
descriptor blocks would be metadata the driver has to step over for nothing.

The three feature words are checked at mount and anything outside what the
driver implements stops the mount. It honours `filetype` (an entry carries
the type, so readdir needs no inode read) and `sparse_super`, and accepts
`large_file` and `ext_attr`.

### Reaching it from the host

e2fsprogs takes a `?offset=` suffix on the device name, and every one of its
tools understands it. That is the whole trick, and it is why no partition is
ever extracted with `dd` and nothing needs root or a loop device:

```bash
debugfs -w "hd.img?offset=1048576"
e2fsck  -fn "hd.img?offset=1048576"
```

`tools/fsimg.sh` is the one place that knows this. Makefiles and test suites
call it (`$(FSIMG) put kernel.rom /KERNEL.ROM`) rather than spelling out an
offset each, and `tools/fsimgtest.sh` tests it — including that its exit
status is right, because a command that did its work and exited 1 anyway
stops every caller chaining on `&&`.

One thing it knows that is easy to get wrong: **`e2fsck -fn` reports a
superblock whose free counts disagree with the bitmaps and still exits 0.**
A test judging by exit status alone calls that volume clean, which is
exactly the fault a driver that miscounts would leave. `fsimg fsck` treats
"clean" as status 0 *and* nothing said.

### What is implemented

**In the boot ROM** (`bootrom/bootrom.c`), read-only and about 200 lines:
the superblock, group 0's descriptor, one inode, and a block map of direct
and singly indirect blocks — enough to find `KERNEL.ROM` in the root
directory and copy it to address 0.

**In the kernel** (`kernel/fs/ext2.c`), read *and* write over a real block
device, behind the same `struct fs_type` FAT16 sits behind: open, read,
write, truncate, unlink, rename, mkdir, rmdir, chdir, getcwd, readdir,
stat, statfs, utime, bmap, and a consistency check of its own.

Underneath it is a sixteen-block write-back cache, least-recently-used —
64 KB, enough to keep a bitmap, an inode table block, an indirect block and
a directory block all resident through one operation.

Three things in the format are easy to get wrong and are handled explicitly:

**A directory entry's `rec_len` is the whole slot**, which may be bigger
than the name in it, and a deleted entry is absorbed into the one before it.
Walking by anything else — a fixed step, say — works on a fresh directory
and desynchronises on the first deletion.

**`i_blocks` is in 512-byte units**, always, whatever the block size is.
That is not a quirk of the driver; it is the format.

**A zero block pointer is a hole**, not an error. It reads as zeroes and
must not be allocated on the read path, or reading a sparse file fills the
disk.

**The check** (`fsck` in the guest, `FSCTL_CHECK`) is not a summary of
what the driver believes; it recomputes everything from the disk. What
it finds and puts right: blocks nothing reaches, a block claimed twice,
a pointer out of range, an inode no name reaches, a link count unequal
to the names that reach it, a wrong `.` or `..`, and free counts that
disagree with the bitmaps. Four things about it are easy to get wrong
and each cost a run to find:

- **`FSCK_IF_DIRTY` is not advisory.** `main.c` calls the check at boot
  with `FSCK_IF_DIRTY|FSCK_REPAIR` and prints nothing when the volume
  was clean. A check that ignored the flag would silently repair a
  clean volume at every boot -- which is how damage planted for a test
  came to be gone before the test could look for it.
- **The inode TABLE is the authority, not the inode bitmap.** An inode
  with a link count and no deletion time holds a file whatever the
  bitmap says. Trusting the bitmap alone means walking past it,
  deciding the blocks it points at are reached by nothing, and freeing
  them out from under a live file. e2fsck reads the table for the same
  reason.
- **The per-group free counts are their own fact.** Fixing the
  superblock's totals does not fix them, and e2fsck reports them
  separately ("Free inodes count wrong for group #0").
- **`i_blocks` has to follow a pointer that is cut off**, or the volume
  still does not check. The walk counts every block it reaches, data
  and indirect alike, which is exactly what `i_blocks` means.

There is one thing here that FAT had no equivalent of. **A file unlinked
while a program still has it open** cannot be freed yet and is reachable
from no directory, so the superblock keeps the head of a list threaded
through the inodes' own `i_dtime` fields — which is what ext2 has always
used the field for in this state. Mounting walks the list and frees what is
on it, so a machine that stopped with a deleted file open leaks nothing and
does not need `e2fsck` to notice.

### What is deliberately not there

- **Hashed directories**, for the reason above.
- **Updating the superblock backups.** The primary is written; e2fsck
  reconciles. Linux does not update them either, except on resize.

### The byte-order trap, for whoever works on the kernel side

**ext2 is little-endian in every field and this machine is big-endian.**
Every superblock field, every block pointer, every inode number and every
directory entry's `rec_len` goes through `le16()`/`le32()` and their write
counterparts, which work a byte at a time and so are indifferent to
alignment as well. Nothing is read by casting a pointer.

Sector *data*, by contrast, needs no swap at all: a big-endian store of each
word reproduces the media byte for byte. Byte streams and word values want
opposite code, and getting it backwards is self-consistent — it passes a
write-then-read-back test in both directions while writing a byte-swapped
image to the media. That already happened once here (`t3-ata`), and it
surfaced only when the boot ROM tried to load something the host had
written. Test against images the host made and can still read afterwards;
`e2fsck` and `debugfs` are the verification a round trip cannot give you.


---


---


---

## 9. Not there yet

The system that runs on this machine is described in [`os.md`](os.md), and
what remains to be built is listed in [`progress.md`](progress.md). Two
items belong here rather than there, because they are decisions about the
machine:

- **Permissions and ownership needed a filesystem that could hold them.**
  FAT cannot (§8), which is the largest single reason the machine's
  filesystem is ext2. Both halves are built now: the disk records an
  owner, a group and a mode, and every path a system call takes is
  checked against them -- see `os.md`, "What is enforced". This is
  recorded here because it was a decision about the machine and not
  about a filesystem: the alternative was permissions that FAT could
  only pretend to store.
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
