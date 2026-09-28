#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fscarry.py - copy directory trees from one disk image to another,
# keeping every file's bytes, mode, owner and group.
#
#   fscarry.py FROM.img TO.img /home /root ...
#
# This is how `make image` keeps what cannot be built: a home directory
# holds what somebody made on the machine -- sources, an ssh key, shell
# history -- and no Makefile can produce it again. What the build DID
# put there (a shipped ~/.profile, say) is replaced by the owner's own
# copy, because the owner's copy is the one they have been using.
#
# debugfs rules this follows, each learned the hard way (see fsimg.sh):
# `mkdir` only where nothing is there, since mkdir over an existing
# name leaks an inode; `rm` before `write`, since write refuses an
# existing name and rm is the removal that frees blocks; and
# `sif ... mode` gets the whole i_mode, type bits included.

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsinv  # noqa: E402


def q(p):
    if '"' in p:
        sys.exit("fscarry: cannot express a name containing a double quote: %r" % p)
    return '"%s"' % p


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: fscarry.py FROM.img TO.img DIR...")
    src, dst, roots = sys.argv[1], sys.argv[2], sys.argv[3:]
    sdev = "%s?offset=%d" % (src, fsinv.offset(src))
    ddev = "%s?offset=%d" % (dst, fsinv.offset(dst))

    have = {r[0]: r for r in fsinv.inventory(dst, sums=False)}
    rows = [r for r in fsinv.inventory(src, sums=False)
            if any(r[0] == d or r[0].startswith(d.rstrip("/") + "/") for d in roots)]
    # the roots themselves too, for their modes and owners
    rows.sort(key=lambda r: r[0])

    cmds = []
    n = 0
    with tempfile.TemporaryDirectory() as tmp:
        files = [r for r in rows if r[1] == "f"]
        # pull every file out of the source in one session
        fsinv.debugfs(sdev, ['dump %s "%s/%d"' % (q(p), tmp, i)
                             for i, (p, *_) in enumerate(files)])
        idx = {r[0]: i for i, r in enumerate(files)}
        for p, k, mode, uid, gid, size, extra in rows:
            m = int(mode, 8)
            if k == "d":
                if p not in have:
                    cmds.append("mkdir %s" % q(p))
                full = 0o040000 | m
            elif k == "f":
                if p in have:
                    cmds.append("rm %s" % q(p))
                local = "%s/%d" % (tmp, idx[p])
                if not os.path.exists(local):
                    sys.exit("fscarry: could not read %s from %s" % (p, src))
                cmds.append("write %s %s" % (q(local), q(p)))
                full = 0o100000 | m
                n += 1
            elif k == "l":
                if p in have:
                    cmds.append("rm %s" % q(p))
                cmds.append("symlink %s %s" % (q(p), q(extra[3:])))
                full = 0o120000 | m
            else:
                print("fscarry: skipping %s (type %s)" % (p, k))
                continue
            cmds.append("sif %s mode 0%o" % (q(p), full))
            cmds.append("sif %s uid %d" % (q(p), uid))
            cmds.append("sif %s gid %d" % (q(p), gid))
        r = subprocess.run(["debugfs", "-w", "-f", "-", ddev],
                           input="\n".join(cmds) + "\n",
                           capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines()
            if l.strip() and not l.startswith("debugfs ")]
    if errs:
        sys.exit("fscarry: debugfs said:\n  " + "\n  ".join(errs[:20]))
    print("carried %d files under %s from %s" % (n, " ".join(roots), src))


if __name__ == "__main__":
    main()
