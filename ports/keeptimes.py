#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""keeptimes.py OUT RECORD - give unchanged files in OUT their old mtimes.

RECORD holds "mtime sha1 path" for every file OUT held last time. A file
whose content is what it was then, but whose mtime is newer, gets the
old mtime back; then RECORD is rewritten for what OUT holds now. See
keep_times in cross.sh for why.
"""
import hashlib
import os
import sys

out, record = sys.argv[1:]
old = {}
try:
    with open(record) as fh:
        for line in fh:
            t, h, p = line.rstrip('\n').split(' ', 2)
            old[p] = (int(t), h)
except FileNotFoundError:
    pass

rows = []
for root, _, files in os.walk(out):
    for f in files:
        p = os.path.join(root, f)
        if os.path.islink(p):
            continue
        with open(p, 'rb') as fh:
            h = hashlib.sha1(fh.read()).hexdigest()
        st = os.stat(p)
        t = st.st_mtime_ns
        if p in old and old[p][1] == h and old[p][0] < t:
            os.utime(p, ns=(st.st_atime_ns, old[p][0]))
            t = old[p][0]
        rows.append(f"{t} {h} {p}\n")

with open(record + '.tmp', 'w') as fh:
    fh.writelines(rows)
os.replace(record + '.tmp', record)
