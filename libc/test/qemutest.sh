#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# qemutest.sh - a program built for the Sage040, run as a Linux/m68k
# program by qemu-m68k's user-mode emulation, on the workstation.
#
# WHAT THIS ACTUALLY TESTS. This system's system call numbers, calling
# convention and errnos are Linux/m68k's, deliberately, and every other
# check of that claim in this tree is made by something in this tree --
# kernel/abicheck.sh compares uapi.h against a table that also lives
# here. This one is not: qemu-m68k is somebody else's implementation of
# Linux/m68k, written with no knowledge of this project, and if a
# program built for the Sage040 runs under it then the ABI is Linux's
# in a way nothing here could have arranged.
#
# It is also the fast way to run a program for this machine -- seconds
# rather than a minute of booting -- and it is how CLISP's build will
# execute the target binary it has to run partway through (task 48).
#
# Static only. A dynamic program asks for /lib/ld.so, which is the
# Sage040's loader at the Sage040's path.

set -u

cd "$(dirname "$0")"
TOPDIR=$(cd ../.. && pwd)
. "$TOPDIR/ports/cross.sh"

QEMU_USER=${QEMU_USER:-qemu-m68k}
WORK=${SAGE_SCRATCH:-/tmp/scratch}/qemuuser
rm -rf "$WORK"; mkdir -p "$WORK"

pass=0
fail=0
check() {
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}

if ! command -v "$QEMU_USER" > /dev/null; then
    echo "qemutest: no $QEMU_USER on this machine."
    echo "  It is qemu's linux-user emulation, packaged separately from"
    echo "  qemu-system-m68k (Arch: qemu-user-static, Debian:"
    echo "  qemu-user-static or qemu-user)."
    echo "RESULT: SKIP"
    exit 0
fi

# A program that exercises more than write(): the file system calls,
# the clock, the environment, memory, and an errno coming back.
cat > "$WORK/abi.c" <<'EOF'
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char buf[64];
    struct stat st;
    int fd, n;
    void *p;

    printf("argc=%d argv1=%s\n", argc, argc > 1 ? argv[1] : "(none)");

    /* open/write/close/open/read: the file system calls. */
    fd = open("probe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("open-write failed %d\n", errno); return 1; }
    write(fd, "sage040\n", 8);
    close(fd);

    fd = open("probe.txt", O_RDONLY);
    n = (int)read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    printf("readback=%.*s", n, buf);

    if (stat("probe.txt", &st) == 0) {
        printf("size=%ld\n", (long)st.st_size);
    }

    /* An errno that has to come back as Linux spells it. */
    errno = 0;
    if (open("no-such-file-here", O_RDONLY) < 0) {
        printf("enoent=%d(%s)\n", errno, errno == ENOENT ? "ENOENT" : "?");
    }

    /* The heap, which means brk or mmap. */
    p = malloc(100000);
    memset(p, 0x5a, 100000);
    printf("malloc=%s\n", p ? "ok" : "failed");
    free(p);

    /* getpid, and the clock. */
    printf("pid>0=%s\n", getpid() > 0 ? "yes" : "no");
    printf("time>0=%s\n", time(NULL) > 0 ? "yes" : "no");

    printf("DONE\n");
    return 0;
}
EOF

echo "=== building a STATIC Sage040 program with the qemu entry ==="
# shellcheck disable=SC2086
"$CROSS_CC" -mcpu=68040 -O2 -c "$TOPDIR/libc/crt0-qemu.s" -o "$WORK/crt0-qemu.o" \
    || { echo "RESULT: FAIL"; exit 1; }
# shellcheck disable=SC2086
"$CROSS_CC" -mcpu=68040 -O2 -nostdlib -Wl,-Bstatic \
    -T "$TOPDIR/libc/sage040.ld" -Wl,--build-id=none \
    -Wl,--no-warn-rwx-segments $CROSS_CPPFLAGS -L"$SAGE_LIBC/lib" \
    "$WORK/crt0-qemu.o" "$WORK/abi.c" \
    -Wl,--start-group -lc -llinux -Wl,--end-group -lgcc \
    -o "$WORK/abi" || { echo "RESULT: FAIL"; exit 1; }

file "$WORK/abi" 2>/dev/null | sed 's/^/    /' | cut -c1-100

echo "=== running it under $QEMU_USER ==="
( cd "$WORK" && timeout 60 "$QEMU_USER" ./abi hello > out.txt 2>&1 )
rc=$?
sed 's/^/  | /' "$WORK/out.txt"

echo "=== checks ==="

test "$rc" -eq 0
check "a Sage040 program runs to completion under qemu-m68k" $?

grep -q "^argc=2 argv1=hello$" "$WORK/out.txt"
check "  argc and argv arrive correctly" $?

grep -q "^readback=sage040$" "$WORK/out.txt"
check "  open, write, read and close work" $?

grep -q "^size=8$" "$WORK/out.txt"
check "  and stat agrees about the size" $?

grep -q "^enoent=2(ENOENT)$" "$WORK/out.txt"
check "  a missing file gives errno 2, which is Linux's ENOENT" $?

grep -q "^malloc=ok$" "$WORK/out.txt"
check "  the heap works, so brk or mmap does" $?

grep -q "^pid>0=yes$" "$WORK/out.txt" && grep -q "^time>0=yes$" "$WORK/out.txt"
check "  getpid and time answer" $?

# The file really was written, by the emulated program, on this disk.
test -f "$WORK/probe.txt" && [ "$(cat "$WORK/probe.txt")" = "sage040" ]
check "the file it wrote is on the HOST's filesystem, with the right contents" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
