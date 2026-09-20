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

Selection criterion throughout: **fewest registers to poke**. Deliberately
rejected — NCR 53C94 SCSI (bus phases, CDBs, DMA), DP83932 SONIC (descriptor
areas plus a CAM load), Zilog Z8530 (fiddlier than a 16550 for no gain).

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

Boot protocol: a big-endian ELF32 (`EM_68K`) loaded with `-kernel`, entered at
its ELF entry point with SP at the top of RAM. No ROM, no bootloader, and no
bootinfo block — the OS knows its own machine.

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
| Kconfig / meson entries | `CONFIG_SAGE040`, `CONFIG_MC68901` |
| `hw/display/sm501.c`: guard the PCI variant with `#ifdef CONFIG_PCI` | upstream compiles it unconditionally, so a sysbus-only board fails to link. This board has no PCI bus and does not carry one — `CONFIG_PCI` is confirmed unset in the build. |
| `target/m68k`: optional `m68k_set_iack_handler()` | a vectored controller needs the interrupt-acknowledge cycle upstream documents as absent. Without it the MFP cannot clear the acknowledged channel, and every interrupt repeats forever. ~15 lines. |

---

## 6. Verification

Every device has a bare-metal test that exercises the real hardware path.
`make run` in `tests/`: **10 programs, 98 checks, all passing, ~14 s.**

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

---

## 7. Status

| Stage | State |
|-------|-------|
| Machine, toolchain, boot | ✅ done |
| Console, C runtime, exception vectors, interrupt dispatch | ✅ done |
| Disk, ethernet, video, MMU, timers | ✅ hardware proven by tests |
| Timer + preemption | unblocked — ordinary OS work now |
| Filesystem, processes, virtual memory, framebuffer console | not started |

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

## 9. Open items

1. **Lift the test code into drivers.** `t3`, `t4`, `t7`–`t10` are proven working
   code for every device; they want turning into `ata.c`, `smc91c111.c`,
   `mfp.c`, `sm501.c` with real interfaces.
2. **Pick a scheduler tick.** Timer C or D at /200 with a reload near 123 gives
   ~10 ms. Note the livelock bound documented in the programmer's guide: a tick
   faster than the handler starves the foreground.
3. **Pick a TCP/IP stack** (§8). lwIP is the recommendation; nothing blocks it
   now that the NIC is proven.
4. **Consider upstreaming** the `sm501.c` build fix and the IACK callback.
5. **Hardware.** Nothing in the design needs a bus that cannot be wired by hand:
   a 68040, an MFP, and five memory-mapped peripherals.

---

## 10. Next

1. **A scheduler tick** — timer D at ~10 ms driving a counter, then a scheduler.
   Everything it needs is tested.
2. **An interrupt-driven console** — `t6` has all the pieces; turning the polled
   `uart.c` into a buffered interrupt-driven one is the natural first use of the
   interrupt path.
3. **Then processes** — user/supervisor split, context switch, `trap #0`
   syscalls, with the MMU available when isolation is wanted.
