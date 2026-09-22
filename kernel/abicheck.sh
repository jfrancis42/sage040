#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# abicheck.sh - the system call numbers are Linux/m68k's, checked.
#
# uapi.h says the numbers are Linux's, on purpose, so that a C library
# written for Linux works unchanged. For months the socket calls were
# i386's instead -- all fifteen three too high -- and the private calls
# sat on msgsnd, msgrcv and msgctl. Nothing noticed, because the kernel
# and the programs both took their numbers from the same header and so
# agreed with each other perfectly. A library built on the real table
# does not share the header's mistakes.
#
# So: every __NR_ name that Linux/m68k has must have Linux/m68k's number,
# and a name Linux does not have is this system's own and must be 1000 or
# more, clear of anything Linux will assign. Runs before every link.

cd "$(dirname "$0")"
TABLE=linux-m68k-syscalls.txt
bad=0

grep -oE '#define __NR_[A-Za-z0-9_]+ +[0-9]+' uapi.h |
    awk '{ sub("__NR_", "", $2); print $2, $3 }' > .abicheck.tmp

while read -r name num; do
    linux=$(awk -v n="$name" '!/^#/ && $2 == n { print $1; exit }' "$TABLE")
    if [ -n "$linux" ]; then
        if [ "$linux" != "$num" ]; then
            echo "abicheck: __NR_$name is $num in uapi.h, $linux on Linux/m68k" >&2
            bad=1
        fi
    elif [ "$num" -lt 1000 ]; then
        taken=$(awk -v x="$num" '!/^#/ && $1 == x { print $2; exit }' "$TABLE")
        echo "abicheck: __NR_$name ($num) is not a Linux call, and must be" \
             "1000 or more${taken:+; Linux uses $num for $taken}" >&2
        bad=1
    fi
done < .abicheck.tmp
rm -f .abicheck.tmp

exit $bad
