#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fsinv.py - what is on a disk image, as one line per file, so that two
# images can be compared with diff.
#
#   fsinv.py IMG                 inventory: PATH TYPE MODE UID GID SIZE [TARGET|SHA]
#   fsinv.py IMG --no-sums       the same without content hashes (fast)
#   fsinv.py A B                 what B lacks that A has, and what differs
#
# It reads the ext2 filesystem inside the image with debugfs, through
# the same "?offset=" suffix tools/fsimg.sh uses, and never writes.
#
# Content is compared by SHA-256 of the file's bytes, so a program that
# was rebuilt differently is told apart from one that is simply there.
# Some files legitimately differ between any two images -- logs, the
# machine's own state -- and `compare` lists them separately rather
# than calling them missing; see VOLATILE.

import hashlib
import os
import subprocess
import sys
import tempfile

# Paths whose CONTENT is the machine's own business: written while it
# runs, so two images built minutes apart differ there by design.
VOLATILE = (
    "/var/log/", "/var/run/", "/tmp/", "/root/.ash_history",
    "/root/.bash_history", "/lost+found",
)


def offset(img):
    off = os.environ.get("PART_OFFSET")
    if off:
        return int(off)
    out = subprocess.run(["sfdisk", "-J", img], capture_output=True, text=True)
    if out.returncode == 0:
        import json
        parts = json.loads(out.stdout)["partitiontable"].get("partitions", [])
        if parts:
            return int(parts[0]["start"]) * 512
    return 0


def debugfs(dev, cmds):
    r = subprocess.run(["debugfs", "-f", "-", dev], input="\n".join(cmds) + "\n",
                       capture_output=True, text=True, errors="surrogateescape")
    return r.stdout


def walk(dev):
    """Yield (path, inode, mode, uid, gid, size) for everything under /."""
    todo = ["/"]
    while todo:
        level, todo = todo, []
        out = debugfs(dev, ['ls -p "%s"' % d for d in level])
        # `ls -p` prints /inode/mode/uid/gid/name/size/ per entry. The
        # command echo lines ("debugfs: ls -p ...") say whose they are.
        cur = None
        for line in out.splitlines():
            if line.startswith("debugfs: ls -p "):
                cur = line[len("debugfs: ls -p "):].strip().strip('"')
                continue
            if not line.startswith("/") or cur is None:
                continue
            f = line.split("/")
            if len(f) < 7:
                continue
            ino, mode, uid, gid, name, size = f[1], f[2], f[3], f[4], "/".join(f[5:-2]), f[-2]
            if name in (".", "..") or ino == "0":
                continue
            path = (cur.rstrip("/") + "/" + name) if cur != "/" else "/" + name
            m = int(mode, 8)
            yield path, int(ino), m, int(uid), int(gid), int(size or 0)
            if (m & 0o170000) == 0o040000:
                todo.append(path)


def kind(m):
    return {0o040000: "d", 0o100000: "f", 0o120000: "l",
            0o020000: "c", 0o060000: "b", 0o010000: "p",
            0o140000: "s"}.get(m & 0o170000, "?")


def inventory(img, sums=True):
    dev = "%s?offset=%d" % (img, offset(img))
    rows = sorted(walk(dev))
    files = [r for r in rows if kind(r[2]) == "f"]
    links = [r for r in rows if kind(r[2]) == "l"]
    extra = {}
    if sums and files:
        with tempfile.TemporaryDirectory() as tmp:
            # dump every file in one debugfs session, then hash them
            cmds = ['dump "%s" "%s/%d"' % (p, tmp, ino) for p, ino, *_ in files]
            for i in range(0, len(cmds), 400):
                debugfs(dev, cmds[i:i + 400])
            for p, ino, *_ in files:
                fp = os.path.join(tmp, str(ino))
                try:
                    with open(fp, "rb") as fh:
                        extra[p] = hashlib.sha256(fh.read()).hexdigest()[:16]
                    os.unlink(fp)
                except FileNotFoundError:
                    extra[p] = "UNREADABLE"
    if links:
        out = debugfs(dev, ['stat "%s"' % p for p, *_ in links])
        cur = None
        for line in out.splitlines():
            if line.startswith("debugfs: stat "):
                cur = line[len("debugfs: stat "):].strip().strip('"')
            elif "Fast link dest:" in line and cur:
                extra[cur] = "-> " + line.split("Fast link dest:", 1)[1].strip().strip('"')
    res = []
    for p, ino, m, uid, gid, size in rows:
        k = kind(m)
        res.append((p, k, "%04o" % (m & 0o7777), uid, gid,
                    size if k == "f" else 0, extra.get(p, "")))
    return res


def fmt(r):
    return "%s %s %s %d %d %d %s" % r


def compare(a_img, b_img):
    a = {r[0]: r for r in inventory(a_img)}
    b = {r[0]: r for r in inventory(b_img)}
    volatile = lambda p: any(p == v.rstrip("/") or p.startswith(v) for v in VOLATILE)
    missing = [p for p in sorted(a) if p not in b and not volatile(p)]
    added = [p for p in sorted(b) if p not in a and not volatile(p)]
    differ = []
    for p in sorted(set(a) & set(b)):
        if volatile(p):
            continue
        ra, rb = a[p], b[p]
        what = []
        if ra[1] != rb[1]: what.append("type %s->%s" % (ra[1], rb[1]))
        if ra[2] != rb[2]: what.append("mode %s->%s" % (ra[2], rb[2]))
        if (ra[3], ra[4]) != (rb[3], rb[4]):
            what.append("owner %d:%d->%d:%d" % (ra[3], ra[4], rb[3], rb[4]))
        if ra[6] != rb[6] and ra[1] in "fl": what.append("content")
        if what:
            differ.append("%s  (%s)" % (p, ", ".join(what)))
    print("== in %s, NOT in %s: %d" % (a_img, b_img, len(missing)))
    for p in missing: print("  - " + fmt(a[p]))
    print("== in %s only: %d" % (b_img, len(added)))
    for p in added: print("  + " + fmt(b[p]))
    print("== in both, different: %d" % len(differ))
    for d in differ: print("  ~ " + d)
    return 1 if missing else 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) == 1:
        for r in inventory(args[0], sums="--no-sums" not in sys.argv):
            print(fmt(r))
    elif len(args) == 2:
        sys.exit(compare(args[0], args[1]))
    else:
        sys.exit(__doc__ if False else "usage: fsinv.py IMG [--no-sums] | fsinv.py A B")
