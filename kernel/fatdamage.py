#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""
fatdamage.py - break a FAT16 image in known ways, for fscktest.sh.

Each kind of damage is one a real crash or a buggy driver leaves behind,
and each is one fsck.fat also knows how to find, so the host's checker
can say afterwards whether the guest's put it right:

  lost      a chain allocated in the FAT that no entry points at
  cross     a file whose chain runs into the middle of another's
  broken    a chain with a link to a free cluster
  size      a file claiming more bytes than its chain holds
  orphan    a long-name run whose 8.3 entry has gone
  dotdot    a directory whose ".." names the wrong parent
  fat2      the second FAT copy disagreeing with the first
  dirty     the "not cleanly unmounted" flags set

  fatdamage.py IMAGE OFFSET KIND [ARGS...]
"""
import struct
import sys


class Fat:
    def __init__(self, path, offset):
        self.f = open(path, "r+b")
        self.base = offset
        b = self.read(0, 512)
        (self.bps, self.spc, self.res, self.nfats, self.roots) = \
            struct.unpack_from("<HBHBH", b, 11)
        self.spf = struct.unpack_from("<H", b, 22)[0]
        self.fat = self.res * self.bps
        self.root = (self.res + self.nfats * self.spf) * self.bps
        self.data = self.root + self.roots * 32
        self.cb = self.spc * self.bps

    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def write(self, off, b):
        self.f.seek(self.base + off)
        self.f.write(b)

    def get(self, c, copy=0):
        return struct.unpack("<H", self.read(self.fat + copy * self.spf *
                                             self.bps + 2 * c, 2))[0]

    def set(self, c, v, copies=None):
        for k in (range(self.nfats) if copies is None else copies):
            self.write(self.fat + k * self.spf * self.bps + 2 * c,
                       struct.pack("<H", v))

    def cluster_off(self, c):
        return self.data + (c - 2) * self.cb

    def dir_entries(self, cluster):
        """(offset, 32 bytes) for every slot of a directory."""
        if cluster == 0:
            for i in range(self.roots):
                o = self.root + 32 * i
                yield o, self.read(o, 32)
            return
        while 2 <= cluster < 0xfff8:
            for i in range(self.cb // 32):
                o = self.cluster_off(cluster) + 32 * i
                yield o, self.read(o, 32)
            cluster = self.get(cluster)

    def find(self, name83, cluster=0):
        for o, e in self.dir_entries(cluster):
            if e[0] == 0:
                break
            if e[11] != 0x0f and e[:11] == name83:
                return o, e
        raise SystemExit("fatdamage: no %r" % name83)

    def chain(self, first):
        out = []
        c = first
        while 2 <= c < 0xfff8:
            out.append(c)
            c = self.get(c)
        return out

    def free_run(self, n):
        run = []
        for c in range(1000, 60000):
            if self.get(c) == 0:
                run.append(c)
                if len(run) == n:
                    return run
            else:
                run = []
        raise SystemExit("fatdamage: no free run")


def n83(s):
    base, _, ext = s.partition(".")
    return (base.ljust(8) + ext.ljust(3)).encode()


def main():
    img, off, kind = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    args = sys.argv[4:]
    v = Fat(img, off)

    if kind == "lost":
        run = v.free_run(4)
        for a, b in zip(run, run[1:] + [0xffff]):
            v.set(a, b)
    elif kind == "cross":
        # args: VICTIM OTHER -- VICTIM's chain is pointed into OTHER's
        o, e = v.find(n83(args[0]))
        _, e2 = v.find(n83(args[1]))
        other = v.chain(struct.unpack_from("<H", e2, 26)[0])
        mine = v.chain(struct.unpack_from("<H", e, 26)[0])
        v.set(mine[0], other[len(other) // 2])
        for c in mine[1:]:
            v.set(c, 0)
    elif kind == "broken":
        o, e = v.find(n83(args[0]))
        mine = v.chain(struct.unpack_from("<H", e, 26)[0])
        free = v.free_run(1)[0]
        v.set(mine[0], free)        # into a free cluster
        for c in mine[1:]:
            v.set(c, 0)
    elif kind == "size":
        o, e = v.find(n83(args[0]))
        size = struct.unpack_from("<I", e, 28)[0]
        v.write(o + 28, struct.pack("<I", size + 5 * v.cb))
    elif kind == "orphan":
        # Delete the 8.3 entry behind a long name, and leave the run.
        want = args[0]
        entries = list(v.dir_entries(0))
        for i, (o, e) in enumerate(entries):
            if e[0] == 0:
                break
            if e[11] == 0x0f:
                continue
            name = e[:11].decode("latin-1")
            if name.startswith(want):
                v.write(o, b"\xe5")
                return
        raise SystemExit("fatdamage: no alias starting %r" % want)
    elif kind == "dotdot":
        o, e = v.find(n83(args[0]))
        sub = struct.unpack_from("<H", e, 26)[0]
        for oo, ee in v.dir_entries(sub):
            if ee[:2] == b".." and ee[2:11] == b" " * 9:
                v.write(oo + 26, struct.pack("<H", sub))   # itself
                return
        raise SystemExit("fatdamage: no ..")
    elif kind == "fat2":
        c = v.free_run(1)[0]
        v.set(c, 0x1234, copies=[1])
    elif kind == "dirty":
        f1 = v.get(1)
        v.set(1, f1 & 0x7fff)
        b = bytearray(v.read(0, 512))
        b[0x25] |= 1
        v.write(0, bytes(b))
    else:
        raise SystemExit("fatdamage: what is %r?" % kind)


if __name__ == "__main__":
    main()
