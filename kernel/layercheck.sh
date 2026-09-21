#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# layercheck.sh - the layering rule, enforced instead of asserted.
#
# The shell and the line editor are meant to be programs that happen to
# be linked into the kernel: they reach the system through trap #0 and
# nothing else, so that the day programs run unprivileged they move
# across the boundary unchanged.
#
# That claim stood in three documents for months while it was quietly
# false -- cmd_console had grown a direct call into tty.c, and nothing
# noticed because nothing was looking. This looks. It runs from the
# kernel's Makefile, so the build fails rather than the documentation
# drifting again.
#
# What is allowed, and why each one is not a hole in the rule:
#
#   syscall.h  the system call ABI. The whole point.
#   uapi.h     what crosses the boundary, included by syscall.h.
#   types.h    integer types.
#   errno.h    error numbers and their names. Pure; no state, no hardware.
#   string.h   memcpy and friends. This is the C library.
#   time.h     calendar arithmetic. Pure -- it does not read the clock,
#              it converts what somebody else read.
#   edit.h     the line editor, which is under the same rule itself.
#
# Anything else -- vfs.h, dev.h, tty.h, console.h, kernel.h, a driver --
# means the file has reached around the system call gate, and that is
# what this refuses.

set -u
cd "$(dirname "$0")"

ALLOWED="syscall.h uapi.h types.h errno.h string.h time.h edit.h"
status=0

for f in shell.c edit.c; do
    for inc in $(sed -n 's/^#include "\(.*\)"/\1/p' "$f"); do
        case " $ALLOWED " in
            *" $inc "*) ;;
            *)
                echo "layercheck: $f includes \"$inc\"" >&2
                echo "  $f is a program. It may reach the system only through" >&2
                echo "  trap #0, so the only kernel header it may include is" >&2
                echo "  syscall.h. Allowed besides that, all of them pure:" >&2
                echo "    $ALLOWED" >&2
                echo "  If this file genuinely needs something else, the thing" >&2
                echo "  it needs wants a system call or an ioctl -- not an" >&2
                echo "  include. That is how cmd_console got its TIOCGCONS." >&2
                status=1
                ;;
        esac
    done
done

exit $status
