#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# lineprobe.py - judge the lines apps/lineprobe drew, from a screendump.
#
# Every check is a property a correct line has whatever Bresenham rule
# drew it, so this is not a second copy of the engine's code:
#
#   - drawn from either end, the same pixels
#   - both end points drawn
#   - one pixel for every step along the major axis, and no more
#   - every pixel within half a pixel of the true line
#   - a line clipped at an edge is exactly the on-screen part of the
#     same line drawn in full somewhere else
#   - and nothing lit anywhere that no line explains
#
# Usage: lineprobe.py SHOT.ppm   (prints ok/FAIL lines; exit 1 on a FAIL)

import sys

CELL, PAD, W, H = 80, 10, 640, 480

SPECS = [
    (0, 0, 60, 20), (0, 0, 20, 60), (60, 0, 0, 20),
    (0, 60, 60, 40), (0, 0, 60, 60), (0, 30, 60, 30),
    (30, 0, 30, 60), (5, 5, 5, 5), (0, 0, 60, 59),
    (0, 0, 59, 60), (0, 0, 3, 60), (10, 10, 47, 13),
    (60, 60, 0, 0), (60, 5, 1, 55), (13, 57, 50, 2),
    (0, 0, 1, 0),
]
CLIPS = [
    (-30, 20, 50, 60, 200, 0),
    (320, -20, 360, 30, 200, 40),
    (600, 450, 700, 500, -300, -40),
]


def read_ppm(path):
    data = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    px = data[i:]
    lit = set()
    for y in range(h):
        for x in range(w):
            k = (y * w + x) * 3
            if px[k] or px[k + 1] or px[k + 2]:
                lit.add((x, y))
    return w, h, lit


failures = 0


def check(what, ok):
    global failures
    print(('  ok   ' if ok else '  FAIL ') + what)
    if not ok:
        failures += 1


def in_box(p, x0, y0, x1, y1):
    return x0 <= p[0] < x1 and y0 <= p[1] < y1


def cell_origin(c):
    return (c % 8) * CELL + PAD, (1 + c // 8) * CELL + PAD


def good_line(pix, x0, y0, x1, y1):
    """The properties that do not depend on the rule."""
    dx, dy = x1 - x0, y1 - y0
    dmaj = max(abs(dx), abs(dy))
    if (x0, y0) not in pix or (x1, y1) not in pix:
        return 'an end point is missing'
    if len(pix) != dmaj + 1:
        return '%d pixels, want %d' % (len(pix), dmaj + 1)
    ymaj = abs(dy) > abs(dx)
    seen = {}
    for (x, y) in pix:
        m, n = (y, x) if ymaj else (x, y)
        seen.setdefault(m, []).append(n)
        if dmaj:
            m0, n0 = (y0, x0) if ymaj else (x0, y0)
            dm, dn = (dy, dx) if ymaj else (dx, dy)
            ideal = n0 + (m - m0) * dn / dm
            if abs(n - ideal) > 0.5 + 1e-9:
                return 'pixel (%d,%d) is %.2f off the line' % (x, y,
                                                               abs(n - ideal))
    if any(len(v) != 1 for v in seen.values()):
        return 'two pixels at one step of the major axis'
    return None


w, h, lit = read_ppm(sys.argv[1])
explained = set()

for i, (x0, y0, x1, y1) in enumerate(SPECS):
    fx, fy = cell_origin(2 * i)
    rx, ry = cell_origin(2 * i + 1)
    f = {(x - fx, y - fy) for (x, y) in lit
         if in_box((x, y), fx - PAD, fy - PAD, fx - PAD + CELL, fy - PAD + CELL)}
    r = {(x - rx, y - ry) for (x, y) in lit
         if in_box((x, y), rx - PAD, ry - PAD, rx - PAD + CELL, ry - PAD + CELL)}
    explained |= {(x + fx, y + fy) for (x, y) in f}
    explained |= {(x + rx, y + ry) for (x, y) in r}
    why = good_line(f, x0, y0, x1, y1)
    check('(%d,%d)-(%d,%d): a good line%s' % (x0, y0, x1, y1,
                                             '' if why is None else ': ' + why),
          why is None)
    check('(%d,%d)-(%d,%d): the same pixels drawn from the other end'
          % (x0, y0, x1, y1), f == r)

for (x0, y0, x1, y1, sx, sy) in CLIPS:
    lo_x, hi_x = min(x0, x1), max(x0, x1)
    lo_y, hi_y = min(y0, y1), max(y0, y1)
    clipped = {p for p in lit if in_box(p, lo_x, lo_y, hi_x + 1, hi_y + 1)}
    full = {p for p in lit
            if in_box(p, lo_x + sx, lo_y + sy, hi_x + sx + 1, hi_y + sy + 1)}
    explained |= clipped | full
    why = good_line({(x - sx, y - sy) for (x, y) in full}, x0, y0, x1, y1)
    check('clip (%d,%d)-(%d,%d): the moved copy is a good line%s'
          % (x0, y0, x1, y1, '' if why is None else ': ' + why), why is None)
    onscreen = {(x - sx, y - sy) for (x, y) in full
                if 0 <= x - sx < W and 0 <= y - sy < H}
    check('clip (%d,%d)-(%d,%d): what is on screen is exactly that part of it'
          % (x0, y0, x1, y1), clipped == onscreen and len(clipped) > 0)

stray = lit - explained
check('nothing lit that no line explains (%d stray)' % len(stray), not stray)
sys.exit(1 if failures else 0)
