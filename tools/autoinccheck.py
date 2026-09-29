#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""
autoinccheck.py - does this toolchain still make a move the CPU runs wrong?

gcc 15.2 for m68k made `move.b (%a0)+,(0,%a0,%d1.l)` out of an ordinary
copy loop: a post-incremented source and a destination indexed off the
SAME register. The 68000 family finishes the source's increment before
it computes the destination, so every byte lands one place too far --
nothing fails, the data is simply shifted. It shifted every thread's
TLS in ld.so before it was found. ports/gcc/patches/03 is the fix.

This compiles the loop that showed it, and variations, with every
compiler the tree builds with, at every optimisation level that
matters, and fails if any output moves through an auto-modified
register that the other operand's address also uses. Run first in
`make test`: a toolchain rebuilt from a tree without the patch is
caught before anything is built with it.
"""
import os
import re
import subprocess
import sys
import tempfile

HOME = os.path.expanduser('~')
COMPILERS = [
    ('C',   os.environ.get('M68K_PREFIX', HOME + '/m68k/install') + '/bin/m68k-elf-gcc', 'c'),
    ('C++', os.environ.get('SAGE_CXX', HOME + '/m68k/install-cxx') + '/bin/m68k-elf-g++', 'c++'),
]
LEVELS = ['-O1', '-O2', '-O3', '-Os']

SOURCE = r'''
struct m { unsigned long a, b, c; const void *img; };
void cp8(unsigned long base, const struct m *m)
{
    unsigned char *dp = (unsigned char *)(base + m->a);
    const unsigned char *sp = (const unsigned char *)m->img;
    unsigned long n = m->c;
    while (n--) *dp++ = *sp++;
}
void cp16(unsigned long base, const struct m *m)
{
    unsigned short *dp = (unsigned short *)(base + m->a);
    const unsigned short *sp = (const unsigned short *)m->img;
    unsigned long n = m->c;
    while (n--) *dp++ = *sp++;
}
void cp32(unsigned long base, const struct m *m)
{
    unsigned long *dp = (unsigned long *)(base + m->a);
    const unsigned long *sp = (const unsigned long *)m->img;
    unsigned long n = m->c;
    while (n--) *dp++ = *sp++;
}
void cpdown(unsigned long base, const struct m *m)
{
    unsigned char *dp = (unsigned char *)(base + m->a) + m->c;
    const unsigned char *sp = (const unsigned char *)m->img + m->c;
    unsigned long n = m->c;
    while (n--) *--dp = *--sp;
}
'''

# Motorola syntax, as gcc writes it: move.b (%a0)+,(%a0,%d1.l)
MOVE = re.compile(r'^\s*move\.[bwl]\s+(-\(%(a[0-7]|sp)\)|\(%(a[0-7]|sp)\)\+),(.*)$')


def conflicts(asm):
    bad = []
    for line in asm.splitlines():
        m = MOVE.match(line)
        if not m:
            continue
        reg = m.group(2) or m.group(3)
        if re.search(r'%' + reg + r'\b', m.group(4)):
            bad.append(line.strip())
    return bad


def main():
    # The check has to be able to fail: a known-bad line must be caught.
    assert conflicts('\tmove.b (%a0)+,(%a0,%d1.l)\n'), 'the pattern is broken'
    assert not conflicts('\tmove.b (%a0)+,(%a1)+\n'), 'the pattern is broken'

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, cc, lang in COMPILERS:
            if not os.path.exists(cc):
                print(f'  [SKIP] {name}: no {cc}')
                continue
            src = os.path.join(tmp, 'loops.' + ('c' if lang == 'c' else 'cc'))
            with open(src, 'w') as f:
                f.write(SOURCE)
            for level in LEVELS:
                r = subprocess.run([cc, '-mcpu=68040', level, '-ffreestanding',
                                    '-fno-builtin', '-S', '-o', '-', src],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    print(f'  [FAIL] {name} {level}: did not compile\n{r.stderr}')
                    failed += 1
                    continue
                bad = conflicts(r.stdout)
                if bad:
                    print(f'  [FAIL] {name} {level}: ' + '; '.join(bad))
                    failed += 1
                else:
                    print(f'  [ OK ] {name} {level}: no move through a register '
                          'it also auto-modifies')
    print('RESULT: ' + ('FAIL' if failed else 'PASS'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
