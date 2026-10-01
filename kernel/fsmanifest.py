#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fsmanifest.py DIR - the host's half of libc/test/fsmanifest.c: the same
# lines, made by Linux reading the same volume (a loop mount), so that
# kernel/linuxfstest.sh can compare the two byte for byte. See the C
# file for the fields.

import os
import stat
import sys


def fnv(path):
    h = 0xcbf29ce484222325
    with open(path, 'rb') as f:
        while True:
            b = f.read(65536)
            if not b:
                break
            for c in b:
                h ^= c
                h = (h * 0x100000001b3) & 0xffffffffffffffff
    return '%016x' % h


def main():
    root = os.fsencode(sys.argv[1])
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        for name in dirnames + filenames:
            rel = name if rel_dir == b'.' else os.path.join(rel_dir, name)
            if rel == b'lost+found':
                continue
            full = os.path.join(dirpath, name)
            st = os.lstat(full)
            m = st.st_mode
            t = 'd' if stat.S_ISDIR(m) else 'l' if stat.S_ISLNK(m) else \
                'p' if stat.S_ISFIFO(m) else 'f'
            target = os.fsencode(os.readlink(full)) if t == 'l' else b'-'
            h = fnv(full) if t == 'f' else '-'
            size = 0 if t == 'd' else st.st_size
            line = b'\t'.join([
                rel, t.encode(), b'%04o' % (m & 0o7777),
                b'%d' % st.st_uid, b'%d' % st.st_gid, b'%d' % st.st_ino,
                b'%d' % st.st_nlink, b'%d' % size, b'%d' % int(st.st_mtime),
                h.encode(), target])
            out.append(line)
        # os.walk follows no links; lost+found is skipped at the top only
        if rel_dir == b'.' and b'lost+found' in dirnames:
            dirnames.remove(b'lost+found')
    out.sort()
    sys.stdout.buffer.write(b''.join(l + b'\n' for l in out))


main()
