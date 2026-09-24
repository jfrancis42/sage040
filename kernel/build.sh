#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - build the kernel ON the machine, with the machine's own
# compiler.
#
# The cross build is kernel/Makefile and is the one to use on a
# workstation. This is the same build done natively, and it is a
# separate script rather than that Makefile for reasons that are all
# about what the machine has:
#
#   - `make` here is sbase's, which is a POSIX make. The Makefile uses
#     $(wildcard), $(filter), $(if) and $(shell), none of which a POSIX
#     make has. Porting GNU make would fix that and is worth doing one
#     day; it is not needed to compile a kernel.
#   - `stat` does not exist -- sbase has no stat(1) -- so the size is
#     taken with `wc -c`.
#   - /bin/sh is the small shell, which has no `for`, `case` or command
#     substitution. layercheck.sh and abicheck.sh need a real one, so
#     they are run through bash by name rather than by their #! line.
#
# THE SOURCE LIST IS NOT HERE. It is in SOURCES, which the Makefile
# reads too, so the two builds cannot drift apart.
#
#   ./build.sh           build kernel.elf and kernel.rom
#   ./build.sh install   build, then put it where the boot ROM looks
#   ./build.sh clean     remove the objects and the kernel
#
# Objects are kept, and a file is recompiled when it or ANY header is
# newer than its object. That is coarser than the Makefile's per-header
# dependencies and deliberately so: a wrong answer here is a kernel
# built from stale objects, which is the kind of thing that costs a day.
# Rebuilding more than necessary only costs minutes.

set -u
cd "$(dirname "$0")"

CC=${CC:-gcc}
OBJCOPY=${OBJCOPY:-objcopy}
SIZE=${SIZE:-size}

CPUFLAGS="-mcpu=68040"
CFLAGS="$CPUFLAGS -ffreestanding -nostdlib -nostdinc -O2
 -Wall -Wextra -Werror -fno-builtin -fno-stack-protector
 -DSAGE040_NO_TESTLIB
 -I. -I.. -Idrivers -Inet -I../tests"
LDFLAGS="$CPUFLAGS -ffreestanding -nostdlib -T kernel.ld
 -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map=kernel.map"

SRCLIST=$(sed -e 's/#.*//' SOURCES)

case "${1:-build}" in
clean)
    for f in $SRCLIST; do rm -f "${f%.*}.o"; done
    rm -f kernel.elf kernel.rom kernel.map
    echo "cleaned."
    exit 0
    ;;
build|install) ;;
*)
    echo "usage: build.sh [build|install|clean]" >&2
    exit 1
    ;;
esac

# The rules the cross build checks before it links, checked here too.
# Through bash by name: their #! says /bin/sh, and this machine's
# /bin/sh cannot run them.
bash ./layercheck.sh || exit 1
bash ./abicheck.sh   || exit 1

# The newest header, found once. Anything older than it is rebuilt.
newest=""
for h in *.h drivers/*.h fs/*.h net/*.h ../types.h ../tests/sage040.h; do
    [ -f "$h" ] || continue
    if [ -z "$newest" ] || [ "$h" -nt "$newest" ]; then newest=$h; fi
done

objs=""
built=0
for f in $SRCLIST; do
    o="${f%.*}.o"
    objs="$objs $o"
    if [ -f "$o" ] && [ ! "$f" -nt "$o" ] &&
       { [ -z "$newest" ] || [ ! "$newest" -nt "$o" ]; }; then
        continue
    fi
    case "$f" in
    *.s) x=assembler-with-cpp ;;
    *.c) x=c ;;
    *)   echo "build.sh: do not know how to compile $f" >&2; exit 1 ;;
    esac
    echo "  CC   $f"
    # shellcheck disable=SC2086
    $CC $CFLAGS -x $x -c "$f" -o "$o" || exit 1
    built=$((built + 1))
done
echo "  ($built compiled)"

echo "  LD   kernel.elf"
# shellcheck disable=SC2086
$CC $LDFLAGS $objs -o kernel.elf || exit 1
$SIZE kernel.elf 2>/dev/null

echo "  COPY kernel.rom"
$OBJCOPY -O binary kernel.elf kernel.rom || exit 1
echo "kernel.rom: $(wc -c < kernel.rom) bytes"

if [ "${1:-build}" = install ]; then
    # The boot ROM reads /KERNEL.ROM off the filesystem. Installing is a
    # copy; the machine takes it at the next reset.
    cp kernel.rom /KERNEL.ROM || exit 1
    echo "installed /KERNEL.ROM -- 'reboot' to run it"
fi
