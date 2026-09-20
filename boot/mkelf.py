#!/usr/bin/env python3
"""Wrap a raw m68k binary in a big-endian ELF32 that QEMU's -kernel can load."""
import struct, sys
raw, out = sys.argv[1], sys.argv[2]
BASE = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x1000
code = open(raw, 'rb').read()
off = 52 + 32
eh = struct.pack('>4sBBBBB7xHHIIIIIHHHHHH', b'\x7fELF', 1, 2, 1, 0, 0,
                 2, 4, 1, BASE, 52, 0, 0, 52, 32, 1, 0, 0, 0)
ph = struct.pack('>IIIIIIII', 1, off, BASE, BASE, len(code), len(code), 5, 0x1000)
open(out, 'wb').write(eh + ph + code)
print(f'{out}: {len(eh+ph+code)} bytes, entry 0x{BASE:x}')
