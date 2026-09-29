#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""mkspecs.py - the native gcc's `specs` file: gcc's own, with ours on top.

    mkspecs.py FULL_DUMP OVERRIDES > specs

A file named `specs` in gcc's version directory is not a set of changes.
gcc reads it INSTEAD of initialising its built-in specs (gcc.cc: when
find_a_file finds "specs", read_specs replaces init_spec), so it has to
be complete -- the whole of `gcc -dumpspecs`. Installing only the four
specs this system changes (libc/sage040.specs) made plain `gcc hello.c`
find its start files, and silently emptied every spec it did not name:
`%(asm_cpu_spec)` found nothing, the assembler was run without
`-mcpu=68040`, and every floating-point instruction the 68040 has only
in that mode was refused. `gcc -dumpspecs` looked right throughout,
because it initialises the built-ins itself before printing.

FULL_DUMP is `-dumpspecs` from a driver configured exactly as the native
one -- the cross compiler in install-cxx is (its dump and an x86 build
of the native configuration's are identical) -- and OVERRIDES is
libc/sage040.specs. The result is the dump in its own order, with each
spec OVERRIDES names replaced by OVERRIDES' value, and one more:

`cross_compile` is 1 in any dump made on this host, because the driver
that made it is a cross compiler, and 0 in the native driver's own
built-ins -- the only spec in gcc.cc that depends on
CROSS_DIRECTORY_STRUCTURE. Left at 1 it tells the native driver it is a
cross compiler, which then leaves /usr/lib and /lib out of its search
for start files and libraries: `cannot find crt0-dyn.o`, with the file
sitting in /usr/lib.
"""
import sys


def parse(text):
    """[(name, value)] from specs-file text: `*name:` then the value's
    lines up to a blank one. Comment lines (#) and directives (%) are
    not specs and are dropped."""
    specs, name, lines = [], None, []
    for line in text.split('\n'):
        if name is None:
            if line.startswith('*') and line.endswith(':'):
                name, lines = line[1:-1], []
            continue
        if line == '':
            specs.append((name, '\n'.join(lines)))
            name = None
        else:
            lines.append(line)
    if name is not None:
        specs.append((name, '\n'.join(lines)))
    return specs


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: mkspecs.py FULL_DUMP OVERRIDES')
    full = parse(open(sys.argv[1]).read())
    over = dict(parse(open(sys.argv[2]).read()))
    names = [n for n, _ in full]
    # A dump that lacks the specs a compile needs is not the full dump.
    for need in ('asm', 'asm_cpu_spec', 'cc1', 'link_command'):
        if need not in names:
            sys.exit('mkspecs.py: %s has no *%s: -- not a full -dumpspecs'
                     % (sys.argv[1], need))
    if 'cross_compile' not in names:
        sys.exit('mkspecs.py: %s has no *cross_compile:' % sys.argv[1])
    over.setdefault('cross_compile', '0')
    unknown = [n for n in over if n not in names]
    if unknown:
        sys.exit('mkspecs.py: %s overrides specs gcc does not have: %s'
                 % (sys.argv[2], ', '.join(unknown)))
    out = sys.stdout
    for name, value in full:
        out.write('*%s:\n%s\n\n' % (name, over.get(name, value)))


if __name__ == '__main__':
    main()
