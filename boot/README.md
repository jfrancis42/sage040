# Proof-of-life kernel

A 78-byte bare-metal kernel that prints one line and halts.

**This targets stock QEMU's `virt` machine, not Sage040.** It predates the
Sage040 machine and uses the `virt` board's goldfish-tty for output — an
Android-emulator invention, and exactly the kind of device the rest of this
repository exists to avoid.

It is kept because it is useful: it needs **no patched QEMU**, so it will tell
you whether your cross toolchain works before you go and build the emulator.
If this prints, your compiler, assembler, linker and ELF wrapping are all
sound, and any later problem is in the machine rather than the tools.

```
$ ./run.sh
--- serial output ---
HELLO FROM 68030 FPU=B
```

`B` is 65 + 1 computed in `fp0`, so it also proves the FPU is live.
`./run.sh m68040` runs the same code on a 68040.

Assembled with `vasm` (Motorola syntax) rather than GNU `as`, and wrapped in a
big-endian ELF32 by `mkelf.py` — which is a compact demonstration of the boot
format QEMU's `-kernel` expects, and is the same format Sage040 uses.
