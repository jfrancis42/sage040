#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""
framecheck.py - the largest stack frame in the kernel, against the red
zone the stack limit leaves (kernel/task.c, KSTACK_RED).

The kernel is built with -fstack-limit-register=a5: each function's
prologue allocates its frame and THEN compares the stack pointer with
the limit. So a frame bigger than the red zone can carry the stack
pointer past the limit, past the guard page beneath it and into
whatever memory is below -- and the check, coming after, then traps
with a frame pushed somewhere that is not a stack at all. The red zone
has to be bigger than the biggest frame, with room over for the trap's
own frame and the registers the report saves.

This reads the frames out of the linked kernel, where they are certain,
rather than out of -fstack-usage, which a one-command build does not
leave behind:

    linkw %fp,#-N   lea %sp@(-N),%sp   addaw #-N,%sp   subql #N,%sp

each immediately before a `cmpal %sp,%a5`.

    framecheck.py KERNEL.ELF RED_ZONE_BYTES [OBJDUMP]

Exit 1, naming the function, if a frame does not fit with a kilobyte to
spare, and the five largest; -v prints those whatever happens.
"""
import re
import subprocess
import sys

MARGIN = 1024


def frames(elf, objdump):
    out = subprocess.run([objdump, "-d", elf], capture_output=True,
                         text=True, check=True).stdout
    func, prev = None, None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            func, prev = m.group(1), None
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        ins = parts[2].strip()
        if re.match(r"cmpal %sp,%a5$", ins) and func:
            n = 0
            if prev:
                for pat in (r"linkw %fp,#-(\d+)", r"linkl %fp,#-(\d+)",
                            r"lea %sp@\(-(\d+)\),%sp",
                            r"addaw #-(\d+),%sp", r"addal #-(\d+),%sp",
                            r"subq?l #(\d+),%sp", r"subaw #(\d+),%sp",
                            r"subal #(\d+),%sp"):
                    mm = re.match(pat, prev)
                    if mm:
                        n = int(mm.group(1))
                        break
            yield func, n
        prev = ins


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip().split("\n\n")[2])
        return 2
    elf, red = sys.argv[1], int(sys.argv[2])
    args = [a for a in sys.argv[3:] if a != "-v"]
    objdump = args[0] if args else "m68k-elf-objdump"
    seen = sorted(frames(elf, objdump), key=lambda f: -f[1])
    if not seen:
        print("framecheck: no stack-limit checks in %s -- built without "
              "-fstack-limit-register?" % elf)
        return 1
    worst = seen[0]
    if worst[1] + MARGIN > red or "-v" in sys.argv:
        for name, n in seen[:5]:
            print("framecheck: %6d bytes  %s" % (n, name))
    if worst[1] + MARGIN > red:
        print("framecheck: %s's frame of %d bytes does not fit the %d-byte "
              "red zone with %d to spare" % (worst[0], worst[1], red, MARGIN))
        return 1
    print("framecheck: %d functions checked, the largest frame fits the "
          "%d-byte red zone" % (len(seen), red))
    return 0


if __name__ == "__main__":
    sys.exit(main())
