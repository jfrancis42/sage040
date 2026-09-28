#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""pagecheck.py - find m68k programs whose code and data share a page.

    tools/pagecheck.py [DIR ...]      (default: every directory that
                                       builds something for the machine)

Under QEMU, a store to a 4 KB page it has translated code from throws
that translation away. A global written in a loop that shares a page
with the loop's own code gets the loop retranslated on every pass --
wator's frame took 26 ms instead of 1.5 that way -- and nothing fails,
it is only slow. Every linker script here therefore starts its writable
sections on a new page, and this is what checks they did: the failure
it guards against is otherwise invisible.

It reads section headers, not segments: ld merges code and data into
one RWE segment when they fit, which says nothing about which PAGES
they occupy. Two kinds of writable section are not counted, because
nothing writes them once the program is running: those ld.so fills in
at load (.got, .dynamic, the constructor arrays), and libc.so's
.libgcc, which is code placed in the writable segment on purpose (see
libc/libc-so.ld).

Exit status 1 if anything shares a page.
"""
import os
import struct
import sys

SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
EM_68K = 4
LOAD_TIME = {'.got', '.got.plt', '.dynamic', '.init_array', '.fini_array',
             '.preinit_array', '.ctors', '.dtors', '.data.rel.ro'}
DEFAULT_DIRS = ['apps', 'system', 'auth', 'ldso', 'ports', 'kernel',
                'bootrom', 'tests', 'cube', 'libc']


def shared_pages(path):
    """None if not an m68k executable or shared object, else the set of
    page numbers holding both code and writable data."""
    try:
        with open(path, 'rb') as f:
            d = f.read()
    except OSError:
        return None
    if len(d) < 52 or d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 2:
        return None
    e_type, e_machine = struct.unpack('>HH', d[16:20])
    if e_machine != EM_68K or e_type not in (2, 3):
        return None
    shoff, = struct.unpack('>I', d[32:36])
    shentsize, shnum, shstrndx = struct.unpack('>HHH', d[46:52])
    if not shoff or shstrndx >= shnum:
        return None
    strtab, = struct.unpack('>I', d[shoff + shstrndx * shentsize + 16:
                                   shoff + shstrndx * shentsize + 20])
    code, data = set(), set()
    for i in range(shnum):
        o = shoff + i * shentsize
        name_off, _, flags, addr, _, size = struct.unpack('>6I', d[o:o + 24])
        name = d[strtab + name_off:d.index(b'\0', strtab + name_off)].decode()
        if not flags & SHF_ALLOC or size == 0 or name in LOAD_TIME:
            continue
        pages = set(range(addr >> 12, ((addr + size - 1) >> 12) + 1))
        if flags & SHF_EXECINSTR:
            code |= pages
        elif flags & SHF_WRITE:
            data |= pages
    return code & data


def main():
    top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dirs = sys.argv[1:] or [os.path.join(top, d) for d in DEFAULT_DIRS]
    checked, bad = 0, []
    for root in dirs:
        for d, subdirs, files in os.walk(root):
            subdirs[:] = [s for s in subdirs if s != '.git']
            for f in files:
                p = os.path.join(d, f)
                if os.path.islink(p) or not os.path.isfile(p):
                    continue
                pages = shared_pages(p)
                if pages is None:
                    continue
                checked += 1
                if pages:
                    bad.append((p, pages))
    for p, pages in sorted(bad):
        print('  SHARED %s: page %s' % (os.path.relpath(p, top),
              ', '.join('0x%x' % (n << 12) for n in sorted(pages))))
    print('pagecheck: %d programs, %d with code and data on one page'
          % (checked, len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
