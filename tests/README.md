# Sage040 device tests

Bare-metal programs that exercise each piece of hardware on the Sage040. Every
one runs against the real emulated device — nothing is stubbed out.

**12 programs, all passing.**

## Running

```bash
make run             # build and run everything, with a summary
make run-t7-mfp-irq  # just one
make disasm-t5-mmu   # disassemble one
```

Each test prints its checks and ends with `RESULT: PASS` or `RESULT: FAIL`,
which is what `make run` counts.

A test that needs keys typed at it prints `KEYS-PLEASE` and has a
companion `.keys` file naming them; `runtest.sh` watches for the
handshake and sends each one through QEMU's monitor. Without the
handshake the scancodes would arrive before anything was reading them.

These are **bare metal**: no kernel underneath, supervisor mode, every
register their own. That is the point of them — they establish what the
hardware does, and the drivers in [`../kernel/drivers/`](../kernel/drivers/) are
written against what they proved. `t2` became `ns16550.c`, `t3` became `ata.c`, `t4` became `smc91c111.c`,
`t6`'s interrupt chain and `t7`/`t8` became `mfp.c`, `t10` became
`sm501.c` and `t11` became `m48t59.c`. What is left without a driver is
`t5` (the MMU, which is not a device) and `t9` (the MFP's USART, which
nothing uses yet).

The kernel has its own test, [`../kernel/fstest.sh`](../kernel/), which
drives a console session and then checks the result with the host's own
`mdir`, `mtype` and `fsck.fat` — 31 checks.

Requires the cross toolchain at `~/m68k/install` (see `../toolchain.md`) and the
patched QEMU at `~/m68k/sage040-qemu` (see `../qemu-patch/`).

## What each test proves

| Test | Device | What it actually checks |
|------|--------|-------------------------|
| `t1-cpu.c` | MC68040 | Supervisor mode, VBR, `mulu.l` 32×32→64, and the FPU against **exact IEEE-754 bit patterns** for pi, 1/3 and `fsqrt(2)` |
| `t2-uart.c` | NS16550A | Scratch register, divisor latch behind DLAB, LCR/LSR, and a **local-loopback** run of six byte patterns through the real datapath |
| `t3-ata.c` | ATA taskfile | `IDENTIFY DEVICE`, model, capacity, then **write 512 bytes and read them back**, comparing every byte |
| `t4-net.c` | LAN91C111 | Banks, revision, MAC registers, then **transmits a real ARP request and waits for the reply** from the slirp gateway |
| `t5-mmu.c` | MC68040 MMU | Builds real three-level page tables, **remaps one page** to a different frame, enables the MMU, proves reads *and* writes follow the translation |
| `t6-irq.c` | whole interrupt chain | UART IRQ → MFP GPIP5 → channel 7 → **vector 0x47** → 68040 dispatch → handler → `RTE` |
| `t7-mfp-irq.c` | MC68901 interrupts | Priority, vector generation, IER gating, IPR/ISR write-zero-to-clear, IMR masking, **software EOI and in-service inhibition** |
| `t8-mfp-timers.c` | MC68901 timers | All four timers, live counters, **prescaler ratio measured at exactly 50** for /4 vs /200, and **event-count mode counting real ATA interrupts** on TAI |
| `t9-mfp-usart.c` | MC68901 USART | Transmit verified against the output file, receive verified against bytes the harness feeds in, plus both interrupt channels |
| `t10-sm501.c` | SM501 video | Device ID, register endianness, 16 MiB with no aliasing, **640×480 framebuffer filled and read back** |
| `t12-kbd.c` | Intel 8042 keyboard | Self test, the command byte, and **which scancode set arrives**: `a` as `0x1E` and not `0x1C`, a release as `0x9E`, no `0xF0` prefix. Keystrokes are injected through QEMU's monitor, because `-display none` delivers no keyboard input at all |
| `t11-rtc.c` | M48T59 clock + NVRAM | NVRAM is real memory and does not alias onto the clock, every time field is in BCD range, **the oscillator advances**, a written date reads back, and **30 February rolls into 1 March** |

The interesting ones are `t4` (a genuine network round trip), `t5` (a real table
walk), `t7` (the full interrupt-controller semantics an OS depends on) and `t8`
(timers counting real device interrupts).

## Files

| File | Purpose |
|------|---------|
| `sage040.h` | Register definitions for every device, from the datasheets |
| `crt0.s` | Entry point: mask interrupts, set VBR, clear `.bss`, call `main()`; plus a default handler that prints `!EXC!` instead of dying silently |
| `sage040.ld` | Linker script — vectors at `0x0`, code at `0x400` |
| `uart.c` | 16550 console driver and the test-reporting helpers |
| `mfp.c` | Shared MFP interrupt plumbing: the vectored stub, vector install, enable/mask/EOI helpers |
| `runtest.sh` | Boots one test and stops QEMU as soon as `RESULT:` appears — the guest ends with `STOP`, which halts the CPU but not QEMU |
| `Makefile` | Build and run |

`runtest.sh` gives the guest two serial lines: the 16550 console captured to
`<test>.out`, and the MFP USART captured to `<test>.usart` and fed from
`<test>.usartin` so the receiver can be tested for real.

## Traps these tests were written around

**MMIO access width.** Several QEMU device regions declare a minimum access
size. On this big-endian target a too-narrow access lands in the wrong byte lane
and produces **no output and no error**. Suspect it first when a driver is
silent.

**Endianness differs per device.** All SM501 control registers are
little-endian; everything else matches the CPU. ATA is the subtle one: sector
data is a byte stream that needs no swap, while `IDENTIFY` returns word values
that do — get that backwards and a capacity of 16384 sectors reads as 64.

**A round-trip test cannot prove byte order.** `t3` originally wrote and read
sector data with matching swaps, which is self-consistent while putting a
byte-swapped image on the media. It passed for weeks and only surfaced when the
boot ROM tried to load a payload the host had written. `t3` now also reads a
signature `runtest.sh` plants in the image before boot, which is the only check
that can catch absolute byte order from inside the guest.

**Empty delay loops vanish at `-O2`.** Use the `delay()` helper in
`t8-mfp-timers.c`, which increments a `volatile`.

**Do not out-run the handler.** A timer at /4 with a small reload fires every
~13 µs, faster than the ISR runs, and the machine livelocks. `t7` therefore arms
a timer, stops it, and only then unmasks — leaving exactly one interrupt
pending.
