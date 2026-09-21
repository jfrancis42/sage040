# Boot ROM

Loads a program off the disk and runs it.

```
$ make disk           # 100 MB image: MBR + FAT16 partition
$ make write          # build the payload, copy it in as KERNEL.ROM
$ make boot           # boot it
```

```
Sage040 boot ROM
partition 1 at LBA 2048, type 0x06
KERNEL.ROM  5948 bytes, first cluster 2
image SSP = 0x003FFFF0  PC = 0x00000400
starting

Sage040 wireframe cube
...
```

The Sage040 has no ROM, so this is loaded with QEMU's `-kernel` — but it does
the job a real machine's ROM monitor would, and everything after it comes off
the disk.

## The disk is a real MS-DOS disk

Not a FAT-like format of our own — a genuine partitioned FAT16 volume the host
reads and writes with ordinary tools, no root required:

```
LBA 0          MBR partition table
LBA 64         optional raw image, in the boot gap
LBA 2048       partition 1, type 0x06, FAT16, volume SAGE040
```

```bash
mcopy -o -i hd.img@@1M kernel.rom ::/KERNEL.ROM   # replace the kernel
mdir     -i hd.img@@1M ::/                        # look at the disk
mmd      -i hd.img@@1M ::/SRC                     # it is just a DOS disk
```

`make disk` builds it with `sfdisk` and `mkfs.fat --offset`, and `make write`
is a one-line `mcopy`. `fsck.fat` reports it clean. Requires `mtools`,
`dosfstools` and `util-linux` on the host.

That is the point of using a real format: replacing the kernel is a file copy,
not a `dd` at a magic offset, and anything else you leave on the disk is
readable from Linux without the guest running.

## How the ROM finds the kernel

1. Read sector 0, check the `55 AA` signature, take partition 1's start LBA.
2. Read the partition's boot sector and parse the BPB — bytes per sector,
   sectors per cluster, reserved sectors, number of FATs, root entries, FAT
   size — and from those work out where the FAT, root directory and data area
   begin.
3. Scan the root directory for `KERNEL.ROM`, skipping deleted entries,
   long-name fragments, the volume label and subdirectories.
4. Follow its cluster chain to address 0, caching one FAT sector so a
   sequential run does not re-read it.
5. Jump via the image's reset vectors.

Read-only, FAT16, 8.3 names, root directory only — exactly as much as finding
one file requires, and about 200 lines.

**Every FAT field is little-endian and this machine is not**, so all of it
goes through `le16()`/`le32()`. Sector *data*, by contrast, is a byte stream
and needs no swapping — see the note at the end of this file for what happens
when those two get conflated.

### If there is no filesystem

It falls back to a raw image at **LBA 64**, in the gap between the partition
table and the first partition. That keeps a disk with no filesystem bootable;
`make write-raw` puts one there. All three paths are exercised: file found,
file missing with a raw image present, and neither.

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
| `make disk` | create `hd.img`, 100 MB, MBR + FAT16 |
| `make write` | build the payload and copy it in as `KERNEL.ROM` |
| `make write-raw` | instead put it raw in the boot gap at LBA 64 |
| `make ls` | partition table and directory listing |
| `make fsck` | check the filesystem |
| `make boot` | run the machine — ROM mounts the disk and boots `KERNEL.ROM` |
| `make clean` | remove build artifacts, keep the disk |
| `make distclean` | also remove the disk image |

To boot something other than the cube, point `PAYLOAD_DIR` and `PAYLOAD_ELF`
at it — or just `mcopy` your own file in as `KERNEL.ROM`. The only requirement is that it links at address 0 with a vector table
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
