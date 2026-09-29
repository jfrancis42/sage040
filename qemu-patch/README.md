# QEMU changes for the `sage040` machine

Against **QEMU 11.1.1**. Everything needed to reproduce the emulator from a
pristine source tree.

## What is here

| File | Role |
|------|------|
| `new-files/hw-m68k-sage040.c` | the machine → `hw/m68k/sage040.c` |
| `new-files/hw-misc-mc68901.c` | the MFP device model → `hw/misc/mc68901.c` |
| `new-files/include-hw-misc-mc68901.h` | its header → `include/hw/misc/mc68901.h` |
| `sage040.patch` | edits to fourteen existing files |

## Applying to a fresh tree

```bash
Q=/path/to/qemu-11.1.1
cp new-files/hw-m68k-sage040.c          $Q/hw/m68k/sage040.c
cp new-files/hw-misc-mc68901.c          $Q/hw/misc/mc68901.c
cp new-files/include-hw-misc-mc68901.h  $Q/include/hw/misc/mc68901.h
patch -d $Q -p1 < sage040.patch
```

## What the patch changes, and why

**`hw/m68k/Kconfig`, `hw/m68k/meson.build`** — add `CONFIG_SAGE040`, selecting
`MC68901`, `SERIAL_MM`, `IDE_MMIO`, `SMC91C111`, `SM501`, `M48T59` and
`PCKBD`.

The **M48T59** clock needed no new code: upstream already provides a sysbus
variant, so the machine instantiates `sysbus-m48t59` with `base-year` 2000
and maps its 8 KiB window. That is why the clock is not an MC146818 —
QEMU's MC146818 model is an `ISADevice` (`config MC146818RTC depends on
ISA_BUS`), and this board has no ISA bus. Making that part work would mean
either inventing one or changing the upstream model's parent type, which is
a much larger and far less upstreamable patch than this file is meant to
hold.

**`hw/misc/Kconfig`, `hw/misc/meson.build`** — add `CONFIG_MC68901`.

**`hw/display/sm501.c`** — wrap the PCI variant in `#ifdef CONFIG_PCI`.
Upstream compiles it unconditionally, so a board that uses only the sysbus
variant fails to link unless the whole PCI subsystem is pulled in. This board
has no PCI bus and should not carry one.

**`hw/input/pckbd.c`, `hw/input/Kconfig`** — the same problem and the same
fix, for the keyboard controller. `pckbd.c` provides both `i8042` (an
`ISADevice`) and `i8042-mmio` (a sysbus device, used by the MIPS Jazz
machines), and `config PCKBD` declared `depends on ISA_BUS` — which is
true of the first and not of the second. The ISA half is now guarded by
`#ifdef CONFIG_ISA_BUS` and the dependency is dropped, so a board can
have a PC keyboard controller without acquiring an ISA bus. Jazz never
noticed because it selects `ISA_BUS` for other devices anyway.

**`target/m68k/cpu.h`, `helper.c`, `op_helper.c`** — add an optional
interrupt-acknowledge callback:

```c
void m68k_set_iack_handler(M68kCPU *cpu,
                           void (*handler)(void *opaque, int level),
                           void *opaque);
```

Upstream notes in `op_helper.c` that *"real hardware gets the interrupt vector
via an IACK cycle at this point"* and that no emulated hardware relied on it. A
vectored controller does: the MC68901 clears the acknowledged channel's pending
bit and, in software end-of-interrupt mode, flags it in service. Without the
callback an interrupt repeats forever.

Two details matter and both cost a debugging cycle:

- The callback is invoked **after** `do_interrupt_m68k_hardirq()`, because that
  function reads `env->pending_level` to set the new interrupt mask in `SR`. A
  handler that runs first and lowers the level leaves the CPU with the wrong
  mask.
- The two fields live **below `end_reset_fields`** in `CPUM68KState`. Everything
  above it is zeroed by `cpu_reset()`, and the machine resets the CPU after the
  controller has registered — which silently erased the handler.

**`target/m68k/cpu.c`, `cpu.h`, `fpu_helper.c`, `helper.h`,
`translate.c`, `op_helper.c`** — the MC68040's floating-point unit as
the chip has it, for an operating system's copy of Motorola's FPSP to
be tested against.

The 68040 implements only part of the MC68881/MC68882 instruction set:
FSIN, FETOX, FLOGN, FMOD, FSCALE, FMOVECR and the rest of Table 9-10 of
the MC68040 User's Manual are not in silicon. For one of them the chip
fetches the operand, converts it to extended precision, and takes vector
11 with a **format $2** frame whose PC is the *next* instruction; the
handler's FSAVE then yields a 26-word *unimplemented instruction* state
frame holding the command word and both operands (section 9.6.1, Table
9-16). Upstream QEMU simply computes these instructions for every m68k
CPU, so an FPSP is never reached and never tested.

- **`fpsp-trap`**, a CPU property, off by default: `-cpu
  m68040,fpsp-trap=on` makes those instructions trap as the silicon does.
  Off, nothing changes — QEMU computes them as it always has.
- **FSAVE** writes the unimplemented instruction frame when one is
  pending, and the IDLE frame it always wrote otherwise. The frame's
  layout is the FPSP's own (`fpsp.h` names every field as an offset from
  the frame's end), which is the software written against the real
  chip; where it and Table 9-16 disagree — the table says E1 is always
  set, the E1 definition and the FPSP read E1 as "the source is packed"
  — the FPSP is followed.
- **FRESTORE** steps over the whole frame on a postincrement. It used to
  consume four bytes whatever the frame said, which is right for an idle
  frame only; the FPSP restores frames of 4, 52 and 100 bytes. This part
  applies whether or not `fpsp-trap` is on.

Not modelled, and said so in `fpu_helper.c`: the frame's address field
(0 here; the FPSP reads it only for instructions that write memory),
tag 101 for single- and double-precision denormal sources, and the BUSY
frames of the arithmetic exceptions — QEMU raises no floating-point
arithmetic exception at all, so there is nothing to put in one.

`kernel/fpsptest.sh` boots the machine both ways and checks the FPSP's
answers against QEMU's softfloat and against the host's libm.

## Building

```bash
mkdir build && cd build
$Q/configure --target-list=m68k-softmmu \
    --prefix=$HOME/m68k/sage040-qemu --enable-slirp \
    --disable-docs --disable-werror --disable-guest-agent \
    --disable-tools --disable-vnc --disable-spice
ninja && ninja install
```

Needs `meson` and `ninja`. **`CONFIG_PCI` and `CONFIG_ISA_BUS` must both
stay unset** — confirm with

```bash
grep -E 'CONFIG_(PCI|ISA_BUS)' build/m68k-softmmu-config-devices.mak
```

returning nothing. Two of the four changes above exist precisely so that
stays true.

## Running

```bash
~/m68k/sage040-qemu/bin/qemu-system-m68k -M sage040 -cpu m68040 -m 4 \
    -kernel kernel.elf \
    -serial file:out.txt \
    -chardev file,id=mfpusart,path=usart.txt,input-path=usart.in \
    -serial chardev:mfpusart \
    -display none -no-reboot \
    -drive file=disk.img,format=raw,if=ide \
    -nic user,model=smc91c111
```

The distro QEMU in `/usr/bin` is left untouched.

## Upstreamability

`fpsp-trap` and the FSAVE/FRESTORE frames are plausible upstream too:
they make QEMU's 68040 a platform an operating system's FPSP can be
developed and tested on, which it currently is not, and change nothing
unless asked for.

The `sm501.c` guard, the `pckbd.c` guard and the IACK callback are all the
kind of change upstream would plausibly take. The first two are build
fixes of the same shape — a file providing both a bus-attached and a
sysbus device, compiled unconditionally — and the third adds a facility
whose absence upstream explicitly documents. The machine and the MFP model are
new files and self-contained.

## License

These files are QEMU-derived and are **`GPL-2.0-or-later`**, matching
upstream, rather than the GPL-3 that covers the rest of this repository.

That is deliberate. GPL-2.0-or-later can be used under GPL-3, so it sits
inside a GPL-3 project without friction — but relicensing it to GPL-3 would
make it impossible to offer upstream, because QEMU cannot accept GPL-3 code.
Since the `sm501.c` build fix and the interrupt-acknowledge callback are both
plausibly upstreamable, keeping the option open costs nothing.
