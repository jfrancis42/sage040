#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""portdeps.py - the ports' dependencies, read from their build.sh.

    portdeps.py check            every port that USES another's output
                                 declares it; exit 1 and name any that
                                 does not
    portdeps.py order PORT...    those ports, each after what it needs
    portdeps.py deps PORT        what PORT needs, directly

A port depends on another when its build.sh

  - declares it:       need_ports a b c        (ports/cross.sh)
  - builds it:         "$HERE/../zlib/build.sh"
                       for p in openssl zlib; do "$HERE/../$p/build.sh"

and USES another when it names that port's output tree,
build-NAME-sage040 (or build-NAME-native). A use that is neither
declared nor built is the bug `check` exists for: it works on a machine
where the other port was built already, by luck, and nowhere else.
"""

import os
import re
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTS = os.path.join(TOP, 'ports')


def ports():
    return sorted(p for p in os.listdir(PORTS)
                  if os.path.isfile(os.path.join(PORTS, p, 'build.sh')))


def source(p):
    with open(os.path.join(PORTS, p, 'build.sh')) as f:
        # Comments say things like "build-gmp-sage040 is where..." and
        # are not uses.
        return '\n'.join(l.split('#', 1)[0] if not l.lstrip().startswith('#')
                         else '' for l in f.read().split('\n'))


def declared(p, known):
    s = source(p)
    deps = set()
    for m in re.finditer(r'^\s*need_ports\s+([^\n;&|]+)', s, re.M):
        deps.update(m.group(1).split())
    for m in re.finditer(r'\.\./([a-z0-9-]+)/build\.sh', s):
        deps.add(m.group(1))
    # for p in a b c; do ... "$HERE/../$p/build.sh"
    for m in re.finditer(r'for (\w+) in ([a-z0-9 -]+); do(.*?)done', s, re.S):
        var, words, body = m.group(1), m.group(2), m.group(3)
        if re.search(r'\.\./\$\{?%s\}?/build\.sh' % var, body):
            deps.update(words.split())
    deps.discard(p)
    return {d for d in deps if d in known}


def used(p, known):
    s = source(p)
    uses = set(re.findall(r'build-([a-z0-9-]+?)-(?:sage040|native)\b', s))
    uses.discard(p)
    return {u for u in uses if u in known}


def closure(p, known):
    out, todo = set(), [p]
    while todo:
        for d in declared(todo.pop(), known):
            if d not in out:
                out.add(d)
                todo.append(d)
    return out


def order(want, known):
    seen, out = set(), []

    def visit(p, stack):
        if p in seen:
            return
        if p in stack:
            sys.exit('portdeps: a cycle: ' + ' -> '.join(stack + [p]))
        for d in sorted(declared(p, known)):
            visit(d, stack + [p])
        seen.add(p)
        out.append(p)

    for p in want:
        visit(p, [])
    return out


def main():
    known = set(ports())
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'check':
        bad = 0
        for p in sorted(known):
            # Declared directly or through a dependency: curl builds
            # openssl, so git, which builds curl, may use openssl.
            missing = used(p, known) - closure(p, known)
            if missing:
                print('ports/%s/build.sh uses %s and does not declare %s '
                      '(need_ports)' % (p, ', '.join(sorted(missing)),
                                        'it' if len(missing) == 1 else 'them'))
                bad = 1
        print('portdeps: %d ports, every use declared' % len(known)
              if not bad else 'portdeps: FAILED')
        return bad
    if cmd == 'order':
        # Only the ports asked for, in an order that respects every
        # dependency among them (what they need but were not asked for
        # is built by need_ports, not installed).
        want = sys.argv[2:]
        full = order(want, known)
        print(' '.join(p for p in full if p in want))
        return 0
    if cmd == 'deps':
        print(' '.join(sorted(declared(sys.argv[2], known))))
        return 0
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main())
