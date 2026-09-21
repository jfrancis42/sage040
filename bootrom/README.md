# Boot ROM

Loads a program off the disk and runs it.

```
$ make write          # put the cube on the disk at sector 0
$ make boot           # boot it
```

```
Sage040 boot ROM
reading 256 sectors from LBA 0 to 0x00000000
....
image SSP = 0x003FFFF0  PC = 0x00000400
starting

Sage040 wireframe cube
...
```

The Sage040 has no ROM, so this is loaded with QEMU's `-kernel` — but it does
the job a real machine's ROM monitor would, and everything after it comes off
the disk.

## The payload format

Deliberately the simplest thing that works: a **raw binary with a 68k vector
table at its start**, exactly what `objcopy -O binary` produces from a program
linked at address 0. No filesystem, no partition table, no header. Sector 0 is
the image.

The first two longs of that table are the 68000 reset vectors:

```
offset 0    initial supervisor stack pointer
offset 4    initial program counter
```

So the boot ROM needs to know nothing about the payload — not even where it
starts. It reads those two longs, points `VBR` at the table, loads the stack
pointer and jumps. That is how a 68000 boots from ROM, and it means this
loader keeps working unchanged when the payload stops being a demo and becomes
a kernel.

A blank disk reads back as zeros, so the loader range-checks both values and
says what is wrong rather than jumping into nothing.

## Memory layout

The payload lands at address 0, so the boot ROM cannot live there:

```
0x00000000 - 0x001fffff   payload, loaded from disk
0x00200000 - 0x003fffff   boot ROM, stack at the top of RAM
```

`BOOT_SECTORS` (default 256, so 128 KB) sets how much is read. `make write`
refuses if the payload is larger and tells you what to rebuild with:

```
make BOOT_SECTORS=512 write boot
```

## Targets

| | |
|---|---|
| `make` | build `bootrom.elf` |
| `make disk` | create `hd.img`, 100 MB |
| `make write` | build the payload, flatten it, write it from sector 0 |
| `make boot` | run the machine — ROM loads sector 0 and jumps to it |
| `make clean` | remove build artifacts, keep the disk |
| `make distclean` | also remove the disk image |

`make write` uses `conv=notrunc`, so it overwrites the first few sectors and
leaves the remaining 100 MB intact.

To boot something other than the cube, point `PAYLOAD_DIR` and `PAYLOAD_ELF`
at it. The only requirement is that it links at address 0 with a vector table
first — which `../tests/sage040.ld` already does.

## A bug this found

The first boot attempt read `SSP = 0x3F00F0FF, PC = 0x00000004` from a disk
that plainly contained `003ffff0 00000400`. Every 16-bit word was swapped.

The cause was in `tests/t3-ata.c`, which had been passing for weeks: it
swapped sector bytes on the way out *and* on the way back in. That is
perfectly self-consistent, so its write-then-read-back check passed — while it
wrote a byte-swapped image to the media. Nothing noticed until something else
had to read the disk.

Sector data is a byte stream and needs no swap; `IDENTIFY` returns word values
and does. `t3-ata` now also verifies a signature the harness writes into the
image before boot, which is the only check that can catch absolute byte order
from inside the guest.
