#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# ext2damage.py - break an ext2 filesystem on purpose, one fault at a
# time, so that fsck has something to find.
#
# It writes the raw image with struct: no e2fsprogs, and nothing shared
# with the kernel's driver.  That is the point -- a damage tool built on
# the same code as the thing under test can only produce damage that
# code is able to express.
#
# Each kind corresponds to one counter in struct fsck_report:
#
#   lost NAME      free the file's directory entry but leave its blocks
#                  marked in use   -> lost_blocks
#   cross A B      point A's first block at B's, so two files share one
#                  block          -> cross_linked
#   badblock NAME  point a block at a number past the end of the volume
#                                 -> bad_blocks
#   links NAME N   set an inode's link count to something it is not
#                                 -> bad_links
#   counts         set the superblock's free counts to a lie
#                                 -> count_mismatch
#   orphan NAME    point a directory entry at an inode that is free
#                                 -> orphan_names
#   dotdot DIR     point a directory's ".." at the wrong inode
#                                 -> dot_entries
#   unattached     mark an inode in use that no name reaches
#                                 -> unattached
#   dirty          clear the superblock's "cleanly unmounted" flag
#
# Usage: ext2damage.py IMAGE OFFSET KIND [ARGS...]

import struct
import sys


class Fs:
    def __init__(self, path, off):
        self.f = open(path, "r+b")
        self.off = off
        sb = self.rd(1024, 1024)
        if struct.unpack_from("<H", sb, 56)[0] != 0xEF53:
            raise SystemExit("ext2damage: not an ext2 filesystem")
        self.bs = 1024 << struct.unpack_from("<I", sb, 24)[0]
        self.inodes_count = struct.unpack_from("<I", sb, 0)[0]
        self.blocks_count = struct.unpack_from("<I", sb, 4)[0]
        self.first_data = struct.unpack_from("<I", sb, 20)[0]
        self.bpg = struct.unpack_from("<I", sb, 32)[0]
        self.ipg = struct.unpack_from("<I", sb, 40)[0]
        rev = struct.unpack_from("<I", sb, 76)[0]
        self.isize = 128 if rev == 0 else struct.unpack_from("<H", sb, 88)[0]
        self.gd_block = self.first_data + 1

    # --- raw access ---------------------------------------------------
    def rd(self, pos, n):
        self.f.seek(self.off + pos)
        return bytearray(self.f.read(n))

    def wr(self, pos, data):
        self.f.seek(self.off + pos)
        self.f.write(data)

    def rblock(self, b):
        return self.rd(b * self.bs, self.bs)

    def wblock(self, b, data):
        self.wr(b * self.bs, data)

    # --- structures ---------------------------------------------------
    def gd(self, g, field):
        """A group descriptor field, BY BYTE OFFSET: block bitmap 0,
        inode bitmap 4, inode table 8."""
        per = self.bs // 32
        blk = self.gd_block + g // per
        d = self.rblock(blk)
        base = (g % per) * 32
        return struct.unpack_from("<I", d, base + field)[0]

    def gd_set16(self, g, field, val):
        per = self.bs // 32
        blk = self.gd_block + g // per
        d = self.rblock(blk)
        struct.pack_into("<H", d, (g % per) * 32 + field, val)
        self.wblock(blk, d)

    def inode_pos(self, ino):
        g = (ino - 1) // self.ipg
        idx = (ino - 1) % self.ipg
        table = self.gd(g, 8)
        return table * self.bs + idx * self.isize

    def inode(self, ino):
        return self.rd(self.inode_pos(ino), self.isize)

    def put_inode(self, ino, raw):
        self.wr(self.inode_pos(ino), raw)

    def blocks_of(self, ino):
        """Direct blocks only -- enough for every file this damages."""
        raw = self.inode(ino)
        return [struct.unpack_from("<I", raw, 40 + 4 * i)[0] for i in range(12)]

    # --- directories --------------------------------------------------
    def entries(self, dino):
        """(name, inode, offset-in-block, block) for each live entry."""
        raw = self.inode(dino)
        size = struct.unpack_from("<I", raw, 4)[0]
        out = []
        for i in range(size // self.bs):
            blk = struct.unpack_from("<I", raw, 40 + 4 * i)[0]
            if blk == 0:
                continue
            d = self.rblock(blk)
            p = 0
            while p + 8 <= self.bs:
                ino, rec, nl, _ft = struct.unpack_from("<IHBB", d, p)
                if rec < 8 or p + rec > self.bs:
                    break
                if ino:
                    out.append((d[p + 8:p + 8 + nl].decode("utf-8", "replace"),
                                ino, p, blk))
                p += rec
        return out

    def find(self, name, dino=2):
        for nm, ino, off, blk in self.entries(dino):
            if nm == name:
                return ino, off, blk
        raise SystemExit("ext2damage: no such name: %s" % name)

    # --- bitmaps -------------------------------------------------------
    def block_bit(self, b, value):
        g = (b - self.first_data) // self.bpg
        bit = (b - self.first_data) % self.bpg
        bm = self.gd(g, 0)
        d = self.rblock(bm)
        if value:
            d[bit >> 3] |= 1 << (bit & 7)
        else:
            d[bit >> 3] &= ~(1 << (bit & 7)) & 0xFF
        self.wblock(bm, d)

    def inode_bit(self, ino, value):
        g = (ino - 1) // self.ipg
        bit = (ino - 1) % self.ipg
        bm = self.gd(g, 4)
        d = self.rblock(bm)
        if value:
            d[bit >> 3] |= 1 << (bit & 7)
        else:
            d[bit >> 3] &= ~(1 << (bit & 7)) & 0xFF
        self.wblock(bm, d)

    def unlink_entry(self, name, dino=2):
        """Remove the directory entry, leaving the inode and its blocks."""
        _ino, off, blk = self.find(name, dino)
        d = self.rblock(blk)
        # Absorb the slot into the entry before it, as a real unlink does.
        p = 0
        prev = None
        while p < off:
            rec = struct.unpack_from("<H", d, p + 4)[0]
            if p + rec >= off:
                prev = p
                break
            p += rec
        rec = struct.unpack_from("<H", d, off + 4)[0]
        if prev is None:
            struct.pack_into("<I", d, off, 0)
        else:
            prev_rec = struct.unpack_from("<H", d, prev + 4)[0]
            struct.pack_into("<H", d, prev + 4, prev_rec + rec)
        self.wblock(blk, d)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    img, off, kind = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    args = sys.argv[4:]
    fs = Fs(img, off)

    if kind == "lost":
        # The name goes; the inode is freed; the blocks stay marked in
        # use and nothing reaches them.
        name = args[0]
        ino, _o, _b = fs.find(name)
        blocks = [b for b in fs.blocks_of(ino) if b]
        fs.unlink_entry(name)
        fs.inode_bit(ino, 0)
        for b in blocks:
            fs.block_bit(b, 1)
        print("lost: %s (inode %d, %d blocks orphaned)" % (name, ino, len(blocks)))

    elif kind == "cross":
        a, b = args[0], args[1]
        ia, _o, _bk = fs.find(a)
        ib, _o2, _bk2 = fs.find(b)
        shared = fs.blocks_of(ib)[0]
        raw = fs.inode(ia)
        struct.pack_into("<I", raw, 40, shared)
        fs.put_inode(ia, raw)
        print("cross: %s now shares block %d with %s" % (a, shared, b))

    elif kind == "badblock":
        name = args[0]
        ino, _o, _b = fs.find(name)
        raw = fs.inode(ino)
        struct.pack_into("<I", raw, 40, fs.blocks_count + 1000)
        fs.put_inode(ino, raw)
        print("badblock: %s points past the end of the volume" % name)

    elif kind == "links":
        name, n = args[0], int(args[1])
        ino, _o, _b = fs.find(name)
        raw = fs.inode(ino)
        struct.pack_into("<H", raw, 26, n)
        fs.put_inode(ino, raw)
        print("links: %s link count set to %d" % (name, n))

    elif kind == "counts":
        sb = fs.rd(1024, 1024)
        fb = struct.unpack_from("<I", sb, 12)[0]
        fi = struct.unpack_from("<I", sb, 16)[0]
        struct.pack_into("<I", sb, 12, fb + 77)
        struct.pack_into("<I", sb, 16, fi + 5)
        fs.wr(1024, sb)
        print("counts: free blocks %d -> %d, free inodes %d -> %d"
              % (fb, fb + 77, fi, fi + 5))

    elif kind == "orphan":
        # A name pointing at an inode that the bitmap says is free.
        name = args[0]
        ino, _o, _b = fs.find(name)
        fs.inode_bit(ino, 0)
        print("orphan: %s names inode %d, which is now free" % (name, ino))

    elif kind == "dotdot":
        d = args[0]
        dino, _o, _b = fs.find(d)
        _ino, off, blk = fs.find("..", dino)
        blk_data = fs.rblock(blk)
        struct.pack_into("<I", blk_data, off, 1)   # inode 1: never a parent
        fs.wblock(blk, blk_data)
        print("dotdot: %s/.. now names inode 1" % d)

    elif kind == "unattached":
        # An inode marked in use that no directory mentions.
        for ino in range(11, fs.inodes_count + 1):
            g = (ino - 1) // fs.ipg
            bit = (ino - 1) % fs.ipg
            bm = fs.rblock(fs.gd(g, 4))
            if not (bm[bit >> 3] >> (bit & 7)) & 1:
                fs.inode_bit(ino, 1)
                raw = bytearray(fs.isize)
                struct.pack_into("<H", raw, 0, 0o100644)
                struct.pack_into("<H", raw, 26, 1)   # claims one link
                fs.put_inode(ino, raw)
                print("unattached: inode %d marked in use, named by nothing"
                      % ino)
                break
        else:
            raise SystemExit("ext2damage: no free inode to strand")

    elif kind == "dirty":
        sb = fs.rd(1024, 1024)
        struct.pack_into("<H", sb, 58, 0)
        fs.wr(1024, sb)
        print("dirty: the volume no longer says it was unmounted cleanly")

    else:
        raise SystemExit("ext2damage: unknown kind: %s" % kind)

    fs.f.close()


if __name__ == "__main__":
    main()
