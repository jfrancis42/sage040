# QEMU changes for the `sage040` machine

Against **QEMU 11.1.1**. Everything needed to reproduce the emulator from a
pristine source tree.

## What is here

| File | Role |
|------|------|
| `new-files/hw-m68k-sage040.c` | the machine → `hw/m68k/sage040.c` |
| `new-files/hw-misc-mc68901.c` | the MFP device model → `hw/misc/mc68901.c` |
| `new-files/include-hw-misc-mc68901.h` | its header → `include/hw/misc/mc68901.h` |
| `sage040.patch` | edits to eight existing files |

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
`MC68901`, `SERIAL_MM`, `IDE_MMIO`, `SMC91C111`, `SM501` and `M48T59`.

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

## Building

```bash
mkdir build && cd build
$Q/configure --target-list=m68k-softmmu \
    --prefix=$HOME/m68k/sage040-qemu --enable-slirp \
    --disable-docs --disable-werror --disable-guest-agent \
    --disable-tools --disable-vnc --disable-spice
ninja && ninja install
```

Needs `meson` and `ninja`. `CONFIG_PCI` must stay unset — confirm with
`grep CONFIG_PCI build/m68k-softmmu-config-devices.mak` returning nothing.

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

The `sm501.c` guard and the IACK callback are both the kind of change upstream
would plausibly take — the first is a build fix, the second adds a facility
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
