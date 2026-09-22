# Sage040 — design

A 68040 workstation built entirely from **real, datasheet-backed silicon**. No
virtio, no paravirtual devices, no PCI. Every part below is a chip you could
buy and solder, chosen so the design could plausibly be built in hardware.

Implemented as a custom QEMU machine, `sage040`.

- **[`programmer-guide.md`](programmer-guide.md)** — how to write code for it
- [`qemu-patch/`](qemu-patch/) — the emulator, reproducible from pristine source
- [`tests/`](tests/) — twelve device tests, `make run`
- **[`os.md`](os.md)** — the operating system that runs on it
- [`emacs.md`](emacs.md) — what it would take to run GNU Emacs
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
`make run` in `tests/`: **12 programs, all passing.** The kernel adds four
more scripted suites, each of which boots the machine and drives it over
its serial line:

| | | |
|---|---|---|
| `kernel/fstest.sh` | 39 checks | the filesystem, verified afterwards with the host's own `mdir`, `mtype` and `fsck.fat` |
| `kernel/edittest.sh` | 27 | the line editor, history, job control, background jobs and `shutdown` |
| `kernel/vmtest.sh` | 15 | memory protection: what a program cannot touch |
| `kernel/nettest.sh` | 13 | ARP, DHCP, ICMP and TCP against a host web server |

106 checks in total. Verifying the guest's writes with the *host's* tools
rather than by reading them back with the same code that wrote them is
deliberate: `t3-ata` is the standing reminder that a round trip cannot
catch a byte-order error, because both directions swap.

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
| Programs (§10) — ELF loader, `spawn`, argv, envp, exit status | ✅ done — `lib/`, `system/`, `apps/` |
| System tick — MC68901 timer D, HZ=100, `nanosleep`, `times` | ✅ done |
| Framebuffer — `/dev/fb0`, point/line/rect/clear/flip, double buffered | ✅ done |
| Text console (§10) — `/dev/fbcon`, 80×30, IBM PC 8×16 font | ✅ done |
| Terminal (§10) — `tty.c`, many sources and sinks | ✅ done |
| Keyboard — Intel 8042, scancode set 1, `/dev/kbd0` | ✅ done |
| Shell — Linux-named commands, environment, PATH, scripts, `/etc/rc` | ✅ done |
| **Virtual memory** — 68040 MMU, per-task address spaces, `uaccess` | ✅ done — `vm.c`, `uaccess.c` |
| **User mode** — programs run unprivileged, faults kill only the program | ✅ done — proven by `vmtest.sh` |
| **Tasks and preemption** — round-robin scheduler, context switch | ✅ done — `task.c`, `taskasm.s` |
| **Blocking** — wait queues, counting semaphores, mutexes | ✅ done — `wait.c` |
| **Signals** — Linux's numbers and `sigaction`, handlers, masks, restart | ✅ done — `signal.c` |
| **Job control** — `&`, `jobs`, `fg`, `bg`, `ps`, `kill`, ctrl-Z | ✅ done |
| Ethernet driver — `struct netdev`, registered as `eth0` | ✅ done, and exercised end to end |
| **TCP/IP (§8)** — ARP, IP, ICMP, UDP, DHCP, TCP, sockets | ✅ done — `kernel/net/` |
| Filesystem (§9) — subdirectories, cwd, `mkdir`/`rmdir`/`chdir` | ✅ done |
| Long file names | ✗ open — §11 |
| `mmap`/`brk` | ✅ done — `vm.c`, `mmap.c` |
| Pipes and redirection, paging, shared libraries | ✗ open — §11 |

**Every hardware dependency is satisfied**, and has been for some time.
What the machine now runs is described in **[`os.md`](os.md)**; what is
still missing is §11.

---

## 8. TCP/IP stack

**Settled: written out, not imported.** The stack is in `kernel/net/` —
ARP, IP, ICMP, UDP, DHCP, TCP and a socket layer — and it works against
real hosts on a real LAN. `os.md` describes what it does; this section is
the record of *why it is not lwIP*, because that was the recommendation
here for a long time and reversing it was a deliberate call.

### Why lwIP was the recommendation

For a machine with a few MB of RAM, lwIP is the obvious answer: 40 KB of
code, a `netif` driver of roughly 250 lines, no dynamic allocation
required, a raw API that avoids threads entirely, and a BSD-socket
compatibility layer on top. It is the standard choice for exactly this
size of system and it would have been quicker.

### Why it was not adopted

The recommendation was right **when the layers below TCP did not exist.**
By the time the question became urgent, ARP, IP, ICMP and UDP were all
written, documented, and wired into the device model and the shell.

lwIP is not a TCP. It is a whole stack, with its own ARP, its own IP and
its own idea of what an interface is. Adopting it at that point meant
*discarding* everything already working and adapting to its device model,
not slotting a layer in on top. The cost had inverted.

What keeps the decision reversible is the socket layer: a program calls
`socket()`, `connect()` and `read()`, and which implementation answers is
not its business. If lwIP is ever wanted, `net/socket.c` is the seam.

### Alternatives, and when each would win

| | When it wins |
|---|---|
| **lwIP** | If the stack below TCP did not already exist, or if IPv6, DNS and DHCP-with-options were all wanted at once |
| **uIP** | A far smaller machine — one segment in flight, no window worth the name |
| **Written out** | What happened: the lower layers existed, and TCP was the only missing piece |

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
- **Initial sequence numbers that cannot be guessed**, RFC 6528, over
  `kernel/random.c` — which is xorshift32 seeded from the clock, the tick
  and the MAC address, and which says at the top of the file, in as many
  words, that it is not a cryptographic generator

**Every item on that list was once in the section below**, as a
deliberate omission justified by the machine only ever talking to its own
LAN. That reasoning held exactly as long as the only network was QEMU's
NAT. Bridging the interface onto a real one turned each omission into a
defect: without reassembly a single lost packet stalls a transfer for a
whole round trip, and an ISN of `jiffies * 7919` is guessable by anyone
who knows roughly when the connection was made.

### What the TCP does not do

Each of these is a decision, and the first four are negotiated options
that a peer works perfectly well without.

- **Window scaling.** It would matter on a path whose bandwidth-delay
  product exceeds 64 KB. The receive buffer is 4 KB, so the window is the
  binding constraint long before the field width is.
- **SACK**, and **timestamps**, and therefore **PAWS**.
- **Path MTU discovery.** The MSS is what fits an ethernet frame.
- **Nagle.** Small writes go out as they are made. A machine with a 4 KB
  send buffer and a human at the other end is not where the
  forty-byte-header problem is solved, and coalescing would make an
  interactive session worse.
- **Keepalives.**
- **A real `TIME_WAIT`.** It is 10 seconds; the specification says 2 MSL,
  which is minutes. This is the one genuine shortcut in the list, and the
  one most likely to matter — a quickly reused port can in principle
  accept a stale segment from a previous connection.

And above it, **there is no resolver**: addresses are numeric everywhere.
DNS over UDP is a few hundred lines on a UDP layer that is already done,
and it is in §11.

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
rename, stat and a directory walk — and **subdirectories**, with `mkdir`,
`rmdir`, a working directory, `chdir` and `getcwd`, and path resolution
through any depth of them.

**That working directory is global rather than per task**, which is a
defect rather than a decision -- see §11.

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

### What is not, yet

- **Long file names.** 8.3 only. Long-name entries the host wrote are
  skipped on a scan rather than misread, so a file created with one is
  still visible by its short name and is not damaged. This is
  period-correct, and it is also **the filesystem's single biggest
  practical limitation**: 43% of GNU Emacs's Lisp files cannot be named
  on this volume at all (measured — `emacs.md`). VFAT long-name entries
  are the answer and are costed in §11.
- **Permissions, ownership, and links.** FAT has nowhere to put any of
  them. `ls -l` shows a mode because `stat` synthesises one.
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
  user mode   programs in their own address spaces:
              lib/  system/{ifconfig,ping,netstat,shutdown,env}  apps/
 ============================== rte / trap #0  ===== the privilege boundary

          shell.c  edit.c        a kernel task, but only syscalls below it
 ------------------------------  trap #0
              syscall.c          40 calls, Linux numbers and convention
      +-----------+-----------+-----------+-----------+
    vfs.c      net/socket.c   task.c      vm.c      exec.c
  paths,       sockets       scheduler   address    ELF loading
  mounts,          |         wait.c      spaces         |
  descriptors      |         signal.c    pmm.c      uaccess.c
      |            |             |        |             |
      |      net/tcp.c udp.c     +--------+-------------+
      |      net/ip.c icmp.c        taskasm.s: the context switch
      |      net/arp.c dhcp.c
      +-----------+-----------+
   fs/fat16.c           dev.c    filesystem types, device registries
      |                   |
 struct blockdev     chardev / netdev / rtcdev / timerdev / fbdev
      |                   |
 drivers/ata.c       drivers/ns16550.c  m48t59.c  mfp.c  sm501.c  i8042.c
                     drivers/smc91c111.c
```

**[`os.md`](os.md) is the full description of everything above the driver
line.** This section is the design record; that document is the reference.

The shell is a **task** now, scheduled like any other, rather than a
function the kernel calls. What has not changed is that it reaches the
machine only through `trap #0`.

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

**This used to say that `syscall_dispatch()` took pointer arguments at
face value, because the MMU was off and there was no address space to
separate.** Both halves have been false for some time. The MMU is on, each
task has its own address space, and every pointer that crosses the gate
goes through `kernel/uaccess.c` — a software table walk against
`current->as`, one page-sized chunk at a time, returning `-EFAULT` rather
than faulting. `kernel/vmtest.sh` is fifteen attempts by a program to
reach something it should not, and exists so that this paragraph cannot
quietly go stale again.

Following `current->as` rather than a global is the whole of one bug:
`exec` used to set the address space around a program's entire run, which
worked while the program ran *inside* the spawning call. The moment a
program became a task of its own, nothing set it, every user pointer
looked like a kernel pointer, and the first `write()` handed the terminal
an address belonging to a different address space.

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

The tick now does three jobs, not one. It counts time for `nanosleep` and
`times`; it **drives preemption**, setting `need_resched` so that
`task_ret_to_user()` switches tasks on the way back to user mode; and it
calls `net_drain()` to move arriving frames off the ethernet card into a
ring. That last one is not an optimisation — the LAN91C111 allocates
transmit buffers from the same page pool that holds received frames, so a
receiver that is never drained stops the machine being able to *send*.

**Preemption happens only on the way back to user mode**, which is what
lets this kernel have no locking at all: it can only be entered by one
task at a time, because a task inside a system call cannot be preempted
out of it. The cost is that a kernel task — the shell — is never preempted
and must block or yield.

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
everything else is the mounted volume. That was written when the
filesystem had one directory, and the promise it made — that the calls
above it would not change when that stopped being true — held: the volume
now resolves through any depth of subdirectory and nothing above `vfs.c`
noticed.

### The serial port

`drivers/ns16550.c` is **a serial port and nothing more**, registered as
`/dev/ttyS0` and as both an input source and an output sink of the
terminal described above.

This section used to say the opposite — that the driver was a terminal,
with canonical mode inside it — and that was true once. The line
discipline moved to `tty.c` when the console grew a second source and a
second sink, because canonical mode belongs to the *terminal*, not to one
of the several devices that can be attached to it. The driver's own header
comment says so, and the two statements disagreed here for a while.

Polled in both directions, on purpose: it works before interrupts are set
up, works inside a panic, and cannot deadlock against the code reporting
the fault. `tty.c` above it does not spin — it sleeps on a wait queue with
a timeout — so polled no longer means burning the processor.

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

**A fault in user mode kills the program, not the machine.** `trap.c`
checks whether the frame came from user mode and, if so, raises `SIGSEGV`
against the current task and exits it; the shell prints what happened and
prompts again. Only a fault in supervisor mode panics, because there is
nothing else it could safely do.

It has to kill rather than return, because the 68040 pushes the address of
the **faulting instruction** — so `rte` re-runs it and faults again
forever. Resolving a fault instead of reporting it is what demand paging
would mean, and that is §11. Note also that the SSW says nothing about
*why*: no bit distinguishes "not mapped" from "write protected" from
"supervisor only", so a handler that needs to know must walk the tables or
use `ptest`.

The full-register report found the first real bug in the kernel: the
memory probe walking off the end of RAM, faulting address in `a0`. A
silent hang is the one outcome worth ruling out.

---

## 11. Open items

Everything below is genuinely open. Items that were on this list and have
since shipped — the scheduler, tasks and preemption, wait queues,
semaphores and mutexes, signals, per-task address spaces, user mode, the
TCP/IP stack, the shell's environment and PATH, shell scripts and
`/etc/rc`, FAT16 subdirectories, job control, and moving the network
tools out of the shell and into `/bin` — are described in `os.md` rather
than kept here as history.

Most of what follows hangs off two things — **a program cannot ask for
memory, and there is no C library** — and the order below reflects that.
Nothing after *A C library* is blocked on anything except the items
above it.

Everything here is intended to be done. There is no "deliberately not
doing this" section any more: paging, shared libraries and the
remaining TCP options used to sit in one, and each was justified by the
machine being small. The machine is not small now — 64 MB of RAM and a
512 MB disk — so the justification went with it.

| | |
|---|---|
| Near-term | interrupt-driven input, DNS, NTP, the NVRAM, static limits |
| **Memory** | ✅ `mmap`, `brk`, `sbrk`, `malloc`, and a 256 MB address space |
| **A C library** | picolibc or newlib over a dozen syscall stubs |
| Pipelines | `pipe`, `dup2`, `SIGPIPE`, and `\|` `>` `>>` `<` in the shell |
| The console | VT102 emulation, `TIOCGWINSZ`, termcap, curses |
| POSIX surface | a dozen small calls (signals, `select`/`poll`, timers, subprocesses ✅) |
| Sockets | ✅ the Linux socket API, signatures and all, and loopback |
| Long file names | VFAT, and why not a different filesystem |
| `fsck` | and a clean-unmount flag to say when it is needed |
| TCP | window scaling, timestamps, SACK, keepalives, a real `TIME_WAIT` |
| Shared libraries | downstream of `mmap` and a libc |
| Paging | downstream of `mmap`, and what makes a big address space affordable |

`emacs.md` is the same list approached from the other end: one real
program, and everything it needs that is not here.

### Near-term, and well understood

1. **Interrupt-driven input.** Both the serial port and the keyboard are
   polled, though both have interrupt lines wired to the MFP and `t6`
   exercises the path. This is no longer a performance problem — `tty.c`
   sleeps on a wait queue with a timeout rather than spinning — so it is
   now a tidiness item rather than a correctness one.

2. **A resolver, and NTP.** Two UDP clients that want doing together,
   because the second wants the first.

   **DNS** first: addresses are numeric everywhere, which is the single
   most visible way this machine is not finished. A stub resolver over
   UDP is a few hundred lines on a UDP layer that already works —
   build the query, send it to the server DHCP already handed us, parse
   the answer section, follow a CNAME, cache what comes back with the
   TTL it came with. `gethostbyname` and then `getaddrinfo` above it,
   in `lib/`, because that is where a ported program looks.

   **NTP** after it. The clock is an M48T59 that reads the host's clock
   under emulation and a dead battery's idea of the time on hardware,
   so the machine's notion of *now* is either borrowed or wrong — and
   every file it writes is stamped with it. SNTP (RFC 4330) is the
   right amount of protocol: one UDP packet out, one back, four
   timestamps, and the offset is
   `((T2 - T1) + (T3 - T4)) / 2`. That is perhaps 200 lines.

   Decisions to make when doing it:
   - **Step or slew.** Step at boot, because the clock may be years
     out and there is nothing running that a jump would upset. Slew
     afterwards if it is ever run as a daemon, because a backward step
     makes file timestamps go backwards and `make` disbelieve its own
     output.
   - **NTP's epoch is 1900, Unix's is 1970.** The difference is
     2,208,988,800 seconds and getting it wrong puts the machine
     seventy years out, which is at least obvious.
   - **The era problem.** NTP's 32-bit seconds field wraps in 2036.
     Worth a comment at minimum, because this machine's own `time_t`
     choices should not quietly inherit somebody else's deadline.
   - **Where it lives.** A program in `system/`, called `ntpdate`, run
     from `/etc/rc` after the interface is up — not a kernel service.
     Setting the clock is `stime()`, which already exists.
   - It should also **write the result to the NVRAM**, which is the
     other open item just below and which is what makes the answer
     survive a reboot.

3. **Use the NVRAM.** The M48T59 brings 8 KiB of it and nothing writes a
   byte. It is the natural home for the network configuration that
   `/etc/rc` currently carries, and for the time NTP last established.

4. **Static limits that will bite.** Eight tasks, eight descriptors per
   task, one filesystem, one partition, one interface. All are constants,
   none is a redesign, and the descriptor limit is the one most likely to
   be hit first.

### Memory: `mmap`, `brk`, `sbrk` and `malloc`

**This is the prerequisite for most of the rest of this section**, and the
first blocker in `emacs.md`. A program's pages are mapped at exec and the
set never changes afterwards; `lib/ulib.h` has no allocator at all, so a
program that wants memory declares an array.

Four things, in dependency order:

1. **A larger user address space. Done:** 256 MB, with page tables
   allocated on demand rather than packed into one page, and a 1 MB
   stack at the top. It was 2 MB because the packing was eight page
   tables to a page; the constant was never the hard part.

2. **`brk`/`sbrk`. Done:** a break per address space, set by exec to
   the page after the image, mapped and unmapped as it moves, with
   Linux's return convention (the old break on failure, never an
   errno).

3. **`mmap`/`munmap`/`mprotect`. Done,** with Linux/m68k's numbers and
   shapes (`mmap2` at 192 with the sixth argument in `a0`, `old_mmap`
   at 90). Anonymous mappings, and file mappings as eager copies:
   exact for `MAP_PRIVATE`, read-only only for `MAP_SHARED`. A real
   shared file mapping still needs a page cache and the reverse mapping
   that paging wants. There is no table of mappings: the page tables are
   the record, which is why partial `munmap` needs no splitting.

4. **`malloc`/`free`/`realloc`/`calloc`.** In the long run, the one
   that comes with a libc (see *A C library* below). For now,
   `lib/malloc.c`: boundary tags, segregated free lists, `mmap` for
   large blocks, and a checker the tests call. It is written to be
   thrown away when the libc arrives, and exists so that nothing
   between here and there waits on it.

The ordering matters because each step is usable on its own: a bigger
address space helps immediately, `brk` alone unlocks `malloc`, and `mmap`
can arrive later without invalidating either.

### A C library

`lib/ulib.c` is not a libc and does not pretend to be. It is a thin
wrapper over the system calls plus `strlen`, `strcmp`, `memset`, `memcpy`
and a few output helpers — enough for the programs in `system/` and
`apps/`, and enough for nothing else. There is a stand-in `malloc`
(`lib/malloc.c`), but no `stdio`, no `printf`, no `qsort`, no `setjmp`,
no locale and no math.

**This is the second-biggest barrier to running anything written by
somebody else**, after the address space. Every portable C program assumes
a hosted implementation.

Three options:

- **Port newlib.** The conventional answer for exactly this situation. It
  targets m68k already, it is designed to sit on a small syscall layer,
  and the interface it expects is about fifteen stubs — `_open`, `_close`,
  `_read`, `_write`, `_lseek`, `_fstat`, `_isatty`, `_sbrk`, `_exit`,
  `_kill`, `_getpid`, `_times`, `_unlink`, `_link`, `_stat`. Most already
  exist here under those names minus the underscore. Its licence is a
  patchwork of BSD-style terms, which is compatible with this project.
- **Port picolibc.** Newlib's smaller descendant, same stub interface,
  better suited to constrained machines, and with a cleaner build. If
  starting today this is probably the better of the two, and the choice
  between them is a genuine decision rather than a toss-up.
- **Grow `ulib` into one.** Tempting, and wrong. A correct `printf` alone
  is a serious piece of work, `setjmp`/`longjmp` needs assembly per ABI,
  and `stdio` buffering has decades of edge cases in it. The only argument
  for writing one is that the result would be small and fully understood —
  which is the same argument that was made, correctly, for writing the
  TCP stack, so it is not absurd. But TCP was written because the layers
  below it already existed and lwIP would have meant discarding them.
  Nothing analogous applies here.

**The recommendation is picolibc or newlib, not a hand-written libc.** A
port converts "write an allocator, a `printf`, `setjmp`, `qsort` and a
string library" into "write fifteen syscall stubs and a linker script",
which is the right trade by a wide margin.

The dependency is one way and strict: a libc needs `sbrk` before `malloc`
can work, and `sbrk` needs the address space item above. So the order is
address space, `brk`, libc — and only then is porting somebody else's
program a question about that program rather than about this system.

One consequence worth naming: today every program statically links its own
`ulib` at ~12 KB. A real libc makes that number much larger and turns
shared libraries from a curiosity into something worth doing.

### Pipelines and redirection

`|`, `>`, `>>` and `<` do not exist, and neither do the pieces underneath
them: there is no `pipe`, no `dup`, no `dup2` and no `fcntl`. The shell
can start several programs now, which is the hard half, so what is left is
mostly plumbing:

- a pipe as a pair of descriptors over a ring buffer, with a reader that
  blocks on a wait queue and a writer that blocks when full
- `SIGPIPE`, which already has a number and a default action
- `dup2`, so the shell can put a descriptor where a program expects it
- redirection parsing in the shell, which is the easy part

The design question worth settling first is whether a pipe is a file in
the VFS or a distinct object that descriptors can point at. The
refcounted open-file layer already in `vfs.c` makes the second
straightforward.

### The console: VT102 emulation, curses, and termcap

**`fbcon.c` is a VT102** (task 11 in `progress.md`). It used to
understand carriage return, backspace, tab and newline plus just enough
of `ESC[2J`, `ESC[H` and `ESC[K` for `clear`, and dropped everything
else -- which is why the line editor builds every movement out of `\r`
and `\b`. The editor still does, because that works on anything; it is
no longer the only thing that works on the screen.

1. **The VT102 emulation -- done.** VT102 rather than bare VT100 because
   it adds insert and delete of lines and characters (`ESC[L`, `ESC[M`,
   `ESC[P`, `ESC[@`), which are exactly what an editor uses to avoid
   repainting, and `vt102` is in every termcap and terminfo already.
   What it does:

   - Cursor: CUP/HVP, CUU/CUD/CUF/CUB, CHA, VPA, CNL/CPL, save and
     restore (`ESC 7`/`ESC 8` and `ESC[s`/`ESC[u`), IND, NEL, RI.
   - Erase: ED and EL with all three parameters, ECH. Erased cells take
     the NORMAL rendition, as a VT102's do (vt102 has no `bce`).
   - Insert and delete: IL, DL, ICH, DCH, insert mode (IRM).
   - **Scroll regions** (DECSTBM) and origin mode (DECOM). Every scroll,
     insert and delete is one blitter copy plus one fill; the SM501
     driver's copy learnt the right-to-left bit for the moves that go
     down or right, which is where they overlap.
   - **The deferred wrap.** Writing the last column leaves the cursor on
     it and wraps only when the next glyph arrives -- terminfo's `xn`.
     Without it, painting an editor's bottom line scrolls the screen.
   - SGR: bold, underline, reverse, and the ANSI colours (30-37, 40-47,
     90-97), because programs send them whatever `TERM` says. The
     console's sixteen colours are palette entries 16-31, so the ones a
     drawing program uses are untouched. Blink is accepted and not shown.
   - DEC special graphics (`ESC ( 0`, and G1 through SO/SI), mapped onto
     the PC font's box pieces; tab stops (HTS, TBC); DECAWM, DECTCEM,
     LNM; RIS and DECALN.
   - **Replies** to DSR (`ESC[6n`, `ESC[5n`) and DA, typed back through a
     small input source -- but ONLY when the screen is the only output.
     With the serial line enabled, the question also went to a real
     terminal that will answer it, and two answers is worse than one.

   The parser follows the DEC one: a C0 control inside a sequence is
   acted on and the sequence continues, CAN and SUB abandon it.

   **`/dev/vcsa`** is Linux's view of the screen -- rows, columns, cursor,
   then a character and attribute byte per cell -- and is how
   `apps/vtcheck` checks what a sequence did. It cannot see pixels, so
   `kernel/vttest.sh` also compares a screenshot taken after a run of
   blitter moves with one taken after `FBCON_REDRAW` redraws everything
   from the character buffer; they must be identical. A forward copy in
   place of the right-to-left one passes every `/dev/vcsa` check and
   fails that one by 7,600 pixels.

   The real constraint remains: each glyph is 128 pixels drawn one at a
   time, so a program that repaints the whole screen per keystroke is
   visibly slow however correct the emulation is. Scroll regions and the
   insert and delete operations are the performance story, not niceties.

2. **`TIOCGWINSZ`, and `SIGWINCH` behind it -- done** (task 12). The
   interesting part is not the ioctl but what the answer should be,
   with two outputs of different sizes live at once. It is **the
   smallest of the enabled outputs**: a program told 30 rows while an
   80x24 terminal is also showing its output paints six rows the
   terminal does not have. The screen answers `TIOCGWINSZ` for itself;
   the serial line cannot be measured, so it is 24x80 until
   `TIOCSWINSZ` says otherwise -- which makes `TIOCSWINSZ` "the line's
   size", not "the answer". `stty` sets it by hand and `resize` asks
   the terminal. Any change to the answer, including switching an
   output on or off with `console`, sends `SIGWINCH` to the foreground
   group. `TERM=vt102` is set by the shell.

   Rejected: making `TIOCSWINSZ` override everything. After `resize` on
   a 50-row xterm, programs would paint 50 rows onto the 30-row screen.

3. **A terminfo or termcap database**, or a deliberate decision not to
   have one. This is the real choice in this section:

   - **Ship terminfo.** Correct, conventional, and drags in either
     ncurses or a reimplementation of its parsing. It also needs a place
     to put the database and long-ish filenames to name the entries.
   - **Compile in one terminal.** Many programs support this — uEmacs
     selects an `ansi.c` driver at build time and needs no database at
     all, and Vim ships builtin entries for exactly this case. Much
     cheaper, and it fits a machine with one console type.

   **The second is almost certainly right here**, at least first. There
   is one console and it is whatever `fbcon.c` implements, so a database
   describing other terminals describes nothing this machine has.

4. **curses or ncurses**, if programs that want it are to be ported. It
   is a substantial library that expects a libc, `malloc`, terminfo and a
   real tty layer — so it sits downstream of §11's memory and libc items
   rather than beside them. Worth noting that the cheapest useful
   full-screen programs deliberately avoid it: BusyBox's `vi` writes ANSI
   escapes directly, and uEmacs has its own driver layer.

The practical sequence is **1, then 2, then decide 3, and treat 4 as
optional** — because a fuller `fbcon.c` plus a compiled-in terminal is
enough to run a real editor, and that is the point of the exercise.
`emacs.md` costs this out against actual editors.

### The POSIX surface a ported program expects

Everything in this subsection exists because `emacs.md` went looking for
it and did not find it. That document costs out one concrete program;
this is the same list as kernel work. **The numbering matches
`emacs.md`'s work list**, so the two can be read against each other.

**(4) Signal handlers, and `sigreturn`.** Signals here have default
actions only and a program cannot install one. Delivering a signal to
user code means building a signal frame on the user stack, returning to
the handler in user mode, and providing a `sigreturn` that unwinds it —
real work in `trap.c` and `execasm.s`, and the largest single kernel item
on this list at perhaps 350 lines. `sigaction`, `sigprocmask` and a
`sigaltstack` for the stack-overflow case. **Almost every interactive
program needs at least `SIGWINCH` and `SIGCHLD`.**

**(5) `select` or `poll`. Done** (`kernel/poll.c`), with Linux/m68k's
three calls. Not built the way this paragraph proposed, with a task
registered on several queues at once. Instead there is one shared
queue that the terminal wakes from the tick, and a short sleep of its
own as a fallback (100 ms, or 20 ms when a socket is watched). That
fallback is needed because the network stack does its protocol work
only when somebody asks, so a socket has nothing that would wake a
queue until a waiting task polls it anyway. Readiness comes from a new
`file_ops->poll`, or from `FIONREAD` for a file that has none.

**(6) Interval timers. Done:** `alarm`, and `setitimer`/`getitimer` with
all three timers. The tick charges each task's time to user or system
from the registers it interrupted, which is what `ITIMER_VIRTUAL`, `ITIMER_PROF`
and `times()`'s `struct tms` needed. `timer_create` is not here.

**(8) The small missing calls.** Individually trivial, collectively the
difference between a program building and not:

| | |
|---|---|
| `fstat` | distinct from `stat`; ported code stats descriptors constantly |
| `access` | can be expressed over `stat` |
| `dup`, `dup2` | needed by redirection; see *Pipelines* above |
| `umask`, `chmod`, `utime` | preserving mode and mtime across a save — and FAT16 has nowhere to put mode, so these must fail honestly rather than silently |
| `getuid`, `getgid`, `getpwuid` | `~` expansion and `user-login-name` |
| `readlink`, `symlink` | no links here, so returning `EINVAL` is the correct answer, not a stub |
| `TIOCGWINSZ` | window size; without it every program assumes 80×24 and the framebuffer console's bottom six rows go unused |
| `fchdir` | Vim uses it |

~300 lines for the lot.

**(9) Subprocesses. Done:** `fork` (an eager copy), `execve`,
`waitpid` with Linux's statuses and options, orphans reaped, and
`/bin/sh`, the kernel's shell built as a program, for `sh -c`.

Items (1) and (2) are *Memory* above; (3) is *A C library*; (7) is *Long
file names* below; (10) and (11) are *The console*. Nothing in
`emacs.md`'s list is missing from this section — which is the point of
numbering them the same way.

### The rest of the Linux socket API

**Done** (`progress.md` task 10). Every call has Linux/i386's number and
signature, with `struct sockaddr` plus `socklen_t`, `struct in_addr`, and
the flags arguments:

| | | |
|---|---|---|
| 359 | `socket` | `SOCK_NONBLOCK`, `SOCK_CLOEXEC` in the type |
| 360 | `socketpair` | `AF_UNIX`, stream only: a pipe each way |
| 361–363 | `bind`, `connect`, `listen` | port 0 picks one; `listen` binds if unbound |
| 364 | `accept4` | `accept()` is it with no flags |
| 365–366 | `getsockopt`, `setsockopt` | `SO_REUSEADDR`, `SO_ERROR`, `SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_TYPE`, `SO_ACCEPTCONN`, `SO_KEEPALIVE` (recorded), `TCP_NODELAY` (always on) |
| 367–368 | `getsockname`, `getpeername` | |
| 369, 371 | `sendto`, `recvfrom` | `MSG_DONTWAIT`, `MSG_PEEK`, `MSG_WAITALL`, `MSG_NOSIGNAL`, `MSG_TRUNC` |
| 370, 372 | `sendmsg`, `recvmsg` | scatter/gather; no ancillary data |
| 373 | `shutdown` | a real half-close |

Blocking calls block until they are satisfied. The old 30-second
`ETIMEDOUT` is gone, and timeouts are `SO_RCVTIMEO`/`SO_SNDTIMEO`. A
non-blocking `connect` returns `EINPROGRESS` and reports through
`SO_ERROR`. `O_NONBLOCK` and `FIONBIO` work on sockets, and so do
`poll`/`select`. **Loopback** (`127.0.0.0/8`, and the machine's own
address) is delivered locally, with or without a configured interface.
A kernel task, `netd`, runs the protocol whether or not anybody is in a
socket call, so a closed connection finishes its FIN handshake and
`TIME_WAIT` on its own, and `close` does not block.

Still not here: named `AF_UNIX` sockets (there is no FAT file type to be
one), `SA_SIGINFO`-style ancillary data, and `getaddrinfo`, which needs
**a resolver** (`progress.md` task 18).

### Long file names

FAT16's 8.3 names are the single biggest practical limitation of the
filesystem, and the cost is not abstract: **43% of GNU Emacs's Lisp files
cannot be named at all** on this volume (measured — see `emacs.md`).

The answer is almost certainly **VFAT long-name directory entries** rather
than a different filesystem. They are an extension to the on-disk format
already implemented, not a replacement for it: a long name is stored in a
run of extra directory entries that carry attribute byte `0x0F`, which
every FAT driver written before them ignores as a volume label. So the
host's `mtools` and `fsck.fat` keep working, the disk stays readable and
writable from Linux without root, and `make write` stays a one-liner —
which is the property that has made this filesystem choice worth keeping.

Roughly 400 lines in `fs/fat16.c`: the checksum that ties a long-name run
to its 8.3 alias, UCS-2 to the kernel's byte strings, generating unique
`NAME~1` aliases, and deleting a whole run rather than one entry.

The alternative — a real filesystem with an inode table — buys permissions,
links, better timestamps and atomic rename, and costs the host
interoperability that makes the current workflow work. Worth a discussion
before anyone starts, not a decision to make in passing.

### fsck, and crash consistency

There is no `fsck` on the machine. The host has one — `fsck.fat`, which
the test suites run after every session precisely because a filesystem
only the kernel can check proves nothing — but the machine cannot check
its own disk, and a guest that cannot is a guest that has to be shut down
cleanly or trusted blindly.

This matters more than it looks, because **writes go out as they are
made**: no journal, no ordering guarantees, and no clean-shutdown flag.
Pulling the plug mid-write leaves exactly what MS-DOS would have left —
lost clusters, cross-linked chains, a directory entry whose size
disagrees with its chain. All recoverable, none currently detected.

What a guest `fsck` has to do, in the order the checks depend on
each other:

- **The boot sector and BPB** against the partition table, and the two
  FAT copies against each other — FAT16 keeps two, and `fs/fat16.c`
  writes both, so a disagreement is the first evidence of a bad write
- **Cluster chains**: every chain terminates, none loops, none runs off
  the end of the table
- **Cross-links**: no cluster claimed by two files, which is the one that
  cannot be repaired without deciding which file to damage
- **Lost clusters**: allocated but claimed by no directory entry, the
  classic `FILE0001.CHK` case
- **Directory sanity**: `.` and `..` present in every subdirectory and
  pointing where they should, given that they are the *only* record of a
  parent; `..` of a directory in the root recorded as cluster 0; sizes
  against chain lengths; names valid 8.3
- **The free count**, recomputed

Then the harder half: **repairing**, and doing it in an order that is
itself crash-safe. A repair interrupted halfway must not leave the volume
worse than it found it.

Two decisions worth making up front:

1. **A clean-unmount flag.** FAT16 has a place for one — the top bits of
   FAT entry 1 — and using it is what lets the machine check the disk
   only when it needs to rather than on every boot. `shutdown` would set
   it and mount would clear it.
2. **A program, not a builtin.** `fsck` belongs in `system/`, like
   `ifconfig` and `ping`, and it needs raw access to `/dev/hda` — which
   works today, since the disk is an ordinary device the shell can open.
   That also means it can be run against an unmounted volume, which is
   the only way to repair one safely.

Being able to run the *host's* `fsck.fat` over the same image afterwards
is the thing that makes this testable, and it is the same argument that
chose FAT16 in the first place.

### TCP's remaining options

Previously listed as deliberate omissions and now on the list to do,
because "a peer works fine without them" is an argument about
correctness and these are about throughput and safety:

- **Window scaling** (RFC 7323). The receive buffer is 4 KB and `cwnd`
  is clamped at 32 KB, so this is only worth having once those grow --
  but they should grow, now that the machine has 64 MB rather than 4.
  Raising the buffers is the first half of this item and the cheaper
  half.
- **Timestamps**, and **PAWS** on top of them. Timestamps also give a
  better RTT sample per round trip than Karn's algorithm can.
- **SACK** (RFC 2018). The reassembly queue already holds out-of-order
  segments, so the receiver half of SACK is mostly a matter of
  reporting what it is already tracking.
- **Keepalives.**
- **A real `TIME_WAIT`.** It is 10 seconds; the specification says 2
  MSL, which is minutes. This is the genuine shortcut in the list: a
  quickly reused port can accept a stale segment from a previous
  connection. It interacts with `SO_REUSEADDR`, so do both together.

### Shared libraries

Moved up from "deliberately not doing this". Everything is statically
linked and each program carries its own copy of `ulib` at ~12 KB, which
is not yet a real cost and becomes one the moment there is a libc worth
the name -- a picolibc-linked editor is not 12 KB.

What it needs, and the order:

1. **Position-independent code.** The 68040 has PC-relative addressing
   with a 16-bit displacement, and `-fPIC` on m68k reaches the GOT
   through `a5`. Conventional, and GCC supports it.
2. **`mmap`**, to place segments. Downstream of *Memory* above.
3. **`.dynamic`, `.got`, `.plt`** in the ELF loader, which currently
   reads program headers and nothing else.
4. **A dynamic linker** -- a program that runs before the program.
5. **Sharing the physical pages** between address spaces, which `vm.c`
   can express and nothing currently asks it to.

### Paging and swapping

Also moved up. There is no demand paging and nothing is ever written to
backing store: a program's pages are all mapped at exec and stay
resident until it exits, and an access fault kills the program rather
than filling a page.

The machine has what this needs -- a full 68040 MMU, a disk, and a fault
handler that already distinguishes user from kernel. What it does not
have is the bookkeeping:

- a fault path that can **resolve** a fault rather than only report it.
  Note that the 68040 pushes the address of the *faulting instruction*,
  so returning with `rte` re-runs it -- which is exactly what demand
  paging wants and exactly what the current handler cannot allow.
- a swap area, a partition or a file
- page-replacement state. The MMU's used and modified bits are there for
  this, and `vm.c` already knows not to touch indirect descriptors.
- a reverse mapping, or a scan, to find what to evict
- pinning, so a page a driver is reading into cannot be taken

The argument for doing it is that it is the natural companion to a large
address space: 256 MB of virtual space is only useful if the unused
parts need not be resident. Sequenced after `mmap`, which shares most of
the machinery.

### Loose ends

- **Consider upstreaming** the `sm501.c` build fix and the IACK callback.
  Both are plausibly of general use, which is why `qemu-patch/` is
  GPL-2.0-or-later.

## 12. Next

1. **A bigger address space, then `brk`, then `malloc`.** In that order,
   because each step is useful on its own and the first is a constant.
   Everything downstream — `mmap`, shared libraries, paging, a real libc,
   and any hope of running a large program — waits on this.
2. **A libc** — picolibc or newlib over those stubs. Together with (1)
   this is what turns "port a program" from a rewrite into a build.
3. **Pipes, `dup2` and redirection.** The shell can already run several
   programs; this is what makes running them *together* possible, and it
   is the largest visible gain for the least new machinery.
4. **A VT102 emulation in `fbcon.c`**, which is what a full-screen
   program needs and what the line editor's `\r`-and-`\b` rule cannot
   stretch to cover.
5. **Long file names**, so the filesystem stops being the thing that
   decides what can be ported — and **`fsck`**, so the machine can check
   the disk it just wrote to.
