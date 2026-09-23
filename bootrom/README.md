# Boot ROM

Loads a program off the disk and runs it.

```
$ make disk           # 512 MB image in the project root: MBR + ext2
$ make write          # build the kernel, copy it in as KERNEL.ROM
$ make boot           # boot it
```

```
Sage040 boot ROM
partition 1 at LBA 2048, type 0x06
KERNEL.ROM  236804 bytes, inode 12
image SSP = 0x003FFFF0  PC = 0x00000400
starting

SuckOS 0.3 on Sage040  (built Sep 22 2026 13:26:08)
...
```

The Sage040 has no ROM, so this is loaded with QEMU's `-kernel` — but it does
the job a real machine's ROM monitor would, and everything after it comes off
the disk.

## The disk is a real ext2 disk

Not an ext2-like format of our own — a genuine partitioned ext2 volume the
host reads, writes and checks with ordinary tools, no root required:

```
LBA 0          MBR partition table
LBA 64         optional raw image, in the boot gap
LBA 2048       partition 1, type 0x83, ext2, volume SAGE040
```

```bash
../tools/fsimg.sh ../hd.img put kernel.rom /KERNEL.ROM  # replace the kernel
../tools/fsimg.sh ../hd.img ls-l /                      # look at the disk
../tools/fsimg.sh ../hd.img fsck                        # check it
```

`fsimg.sh` is the one place that knows how to reach the filesystem inside
the partition; underneath it is e2fsprogs' `?offset=` suffix, which every
one of its tools understands.

The image lives in the **project root**, not here: the ROM boots from it, the
kernel reads and writes it, and the host puts files on it, so it belongs to
the machine rather than to any one of them. Its definition is in
[`../disk.mk`](../disk.mk), which every Makefile includes.

`make disk` builds it with `sfdisk` and `mke2fs -E offset=`, and `make write`
is one line. `e2fsck` reports it clean. Requires `e2fsprogs` and
`util-linux` on the host — and nothing needs root or a loop device.

That is the point of using a real format: replacing the kernel is a file copy,
not a `dd` at a magic offset, and anything else you leave on the disk is
readable from Linux without the guest running.

## How the ROM finds the kernel

1. Read sector 0, check the `55 AA` signature, take partition 1's start LBA.
2. Read the superblock — always at byte 1024 of the filesystem, whatever the
   block size is — check its magic, and take the block size, the inodes per
   group and the inode size from it.
3. Read group 0's descriptor for the inode table, and from that the root
   directory's inode.
4. Walk the root directory **by `rec_len`**, which is the whole slot and not
   the length of the name in it, looking for `KERNEL.ROM`.
5. Copy its blocks to address 0 through its block map: twelve direct
   pointers and one indirect block, which at the 4 KB block size the disk is
   made with reaches 4 MB — so no kernel here needs double indirection.
6. Jump via the image's reset vectors.

Read-only, root directory only, direct and singly indirect blocks — exactly
as much as finding one file requires, and about 200 lines.

FAT16 is deliberately **not** read here. The kernel still mounts a FAT
volume, so a disk from a machine that has never heard of this one is still
readable once the kernel is up; what the ROM has to find is this machine's
own kernel, and that lives on this machine's own disk.

**Every ext2 field is little-endian and this machine is not**, so all of it
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

`BOOT_SECTORS` (default 1920, so 960 KB) bounds the payload on **both**
paths. The raw fallback reads that many sectors from the gap before the
partition, and `make write-raw` refuses if the image is larger and tells
you what to rebuild with:

```
make BOOT_SECTORS=1920 write-raw boot
```

The filesystem path follows the block map and reads only the file's own
size, but it stops at the same limit. This README used to say it had
none; it did, at 128 KB, and a kernel that grew past it stopped booting
with "short read".

## Targets

| | |
|---|---|
| `make` | build `bootrom.elf` |
| `make disk` | create `../hd.img`, 512 MB, MBR + ext2 |
| `make write` | build the kernel and copy it in as `KERNEL.ROM` |
| `make write-cube` | put the cube there instead |
| `make write-raw` | put the cube raw in the boot gap at LBA 64 |
| `make ls` | partition table and directory listing |
| `make fsck` | check the filesystem |
| `make boot` | run the machine — ROM mounts the disk and boots `KERNEL.ROM` |
| `make clean` | remove build artifacts, keep the disk |
| `make distclean` | also remove the disk image |

`make write` asks `../kernel` to install itself, so the kernel owns the file it
puts on the disk. The programs that go alongside it come from
`make programs` at the top level. To boot something else, put your own file
on the disk as `KERNEL.ROM`; the only requirement is that it links at address 0 with a vector
table first, which `../tests/sage040.ld` and `../kernel/kernel.ld` both do.

`make write-cube` is the demonstration this ROM was first written against, and
still a useful way to prove the loader with the kernel out of the picture.

## Byte order

**Sector data is a byte stream and takes no swap; `IDENTIFY` returns 16-bit
values and does.** Getting that backwards is self-consistent — a
write-then-read-back check passes while the image on the media is
byte-swapped — and the first thing it breaks is this ROM, which read
`SSP = 0x3F00F0FF, PC = 0x00000004` from a disk that plainly contained
`003ffff0 00000400`.

`t3-ata` therefore also verifies a signature the harness writes into the
image before boot, which is the only check that can catch absolute byte order
from inside the guest.
