#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# shebangtest.sh - a file beginning "#!" is run by the program it names.
#
# Every check is built so that a SHELL could not have passed it for the
# kernel. A shell given ENOEXEC runs the file itself, as a shell script,
# and the first line of every script here is then a comment -- so the
# output these checks look for can only come from the named interpreter
# having run. That was the state of things before exec learned #!: the
# kernel shell and bash both ran every script themselves, whatever it
# named.
#
# The interpreter is mostly sbase's printf, because printf shows its
# arguments exactly: `#!/bin/printf <%s>\n` prints each argument it is
# handed in brackets on a line of its own, so the argv the kernel built
# is on the screen as it was built.
#
# Runs the scripts from both shells: the kernel's (spawn) and bash
# (fork and execve). They are different paths into exec.c.

set -u
# A write to the console FIFO after QEMU has died raises SIGPIPE, and
# that killed the suite before it printed a single check -- a machine
# that crashed read as silence. Ignored, the write fails and the checks
# say what went wrong.
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-shebang.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "shebangtest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/shebangtest.log"; FIFO="$SCRATCH/sb.fifo"
rm -f "$LOG" "$FIFO"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../ldso || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /etc; fsimg mkdir /s
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
put_shells bash
fsimg put -m 644 ../system/passwd /etc/passwd
for p in printf echo touch cat; do
    fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p
done

T="$SCRATCH/sb.tmp"; rm -rf "$T"; mkdir -p "$T"
script() {                      # script NAME MODE CONTENT
    printf '%b' "$3" > "$T/$1"
    fsimg put -m "$2" "$T/$1" "/s/$1"
}
# One argument after the interpreter, spaces and all: the format below
# is ONE argument, "<%s> %s|\n", so printf takes the script path and the
# first argument two at a time. Split at the space, it would print the
# second half as text.
script one     755 '#!/bin/printf <%s> %s|\\n\necho SHELL-RAN-ONE\n'
script plain   755 '#!/bin/printf <%s>\\n\necho SHELL-RAN-PLAIN\n'
# Blanks after #!, and trailing blanks and a CR on the line, go.
script spaced  755 '#!   /bin/printf    [%s]\\n   \r\necho SHELL-RAN-SPACED\n'
# A script whose interpreter is a script: /s/plain runs /s/nest.
script nest    755 '#!/s/plain\necho SHELL-RAN-NEST\n'
# A chain of scripts: at most four may stand between exec and a real
# program (Linux's BINPRM_MAX_RECURSION). l3 -> l2 -> l1 -> plain is
# four and runs; l5 is six and is ELOOP.
script l1 755 '#!/s/plain\n'
script l2 755 '#!/s/l1\n'
script l3 755 '#!/s/l2\n'
script l4 755 '#!/s/l3\n'
script l5 755 '#!/s/l4\n'
# No execute permission on the script: refused, however good its line.
script noexec  644 '#!/bin/printf <%s>\\n\n'
# An interpreter that does not exist.
script noint   755 '#!/bin/no-such-thing\necho SHELL-RAN-NOINT\n'
# "#!" and nothing after it.
script empty   755 '#!\necho SHELL-RAN-EMPTY\n'
# A '#' script without the '!' is still the shell's to run.
script hashonly 755 '# a comment\necho HASH-ONLY-RAN\n'
# SET-USER-ID ON A SCRIPT IS IGNORED. The script is owned by uid 1000
# and 4755, and its interpreter is touch: the file touch makes is owned
# by touch's EFFECTIVE uid, and the host reads that owner afterwards.
# Run by root, it must be 0. The ELF copy of touch set up the same way
# is the control: its file must be 1000's, or the check could not fail.
# (whoami would be the obvious interpreter, and refuses the script's
# path as an extra argument.)
fsimg mkdir /w
fsimg chown /w 1000:1000
script suid   4755 '#!/bin/touch /w/by-script\n'
fsimg chown /s/suid 1000:1000
script suidb  4755 '#!/bin/touch /w/by-script-bash\n'
fsimg chown /s/suidb 1000:1000
fsimg put -m 4755 ../ports/sbase/bin/touch /s/suidelf
fsimg chown /s/suidelf 1000:1000

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"

wait_for() {                    # wait_for REGEX [TENTHS]
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
# The marker split by quotes, so the echo of the typed line is not it.
run() {                         # run TAG COMMAND
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" 300
}

sleep 5

# --- from the kernel's shell: spawn ---------------------------------
run K1 '/s/one a "b c"'
run K2 '/s/spaced x'
run K3 '/s/nest y'
run K4 '/s/l5'
run K4B '/s/l3 z'
run K5 '/s/noexec'
run K6 '/s/noint'
run K7 '/s/empty'
run K8 '/s/hashonly'
run K9 '/s/suid'
run K10 '/s/suidelf /w/by-elf'
# --- from bash: fork and execve -------------------------------------
printf '/bin/bash\r' >&3
sleep 3
run B1 '/s/one a "b c"'
run B2 '/s/nest y'
run B3 '/s/l5'
run B4 '/s/noint'
run B5 '/s/suidb'
# exec replaces bash itself with the interpreter, so the marker has to
# come from outside: the kernel shell prints it once bash has gone.
printf 'exec /s/plain e1 e2\r' >&3
sleep 3
run B6 'echo back'
printf 'echo ALL-DONE\r' >&3
wait_for 'ALL-DONE' 100
printf 'halt\r' >&3
sleep 2
exec 3>&-; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
rm -f "$FIFO"

CLEAN="$SCRATCH/shebangtest-clean.tmp"
# bash's bracketed-paste switches (ESC [?2004h / l) go too, or no line
# it prints matches whole.
tr -d '\r' < "$LOG" | sed 's/\x1b\[[?0-9;]*[a-zA-Z]//g' > "$CLEAN"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
has() { grep -qxF -- "$1" "$CLEAN"; }
hasre() { grep -qxE -- "$1" "$CLEAN"; }
# Everything the log says between one marker and the next.
between() { awk -v a="$1" -v b="$2" '$0 ~ "^"b"-END" {on=0} on {print} $0 ~ "^"a"-END" {on=1}' "$CLEAN"; }

echo "=== checks: the kernel shell (spawn) ==="
has '</s/one> a|' && has '<b c> |'
check "the rest of the line is ONE argument, then the script path and its arguments" $?
! grep -v 'SHELL-RAN-EMPTY' "$CLEAN" | grep -q 'SHELL-RAN'
check "no script's body was run by a shell: the interpreter ran instead" $?
has '[/s/spaced]' && has '[x]'
check "blanks after #!, trailing blanks and a CR are not part of the line" $?
has '</s/plain>' && has '</s/nest>' && has '<y>'
check "a script's interpreter may be a script: plain /s/nest y" $?
hasre 'K4-END [1-9][0-9]*' && ! between K3 K4 | grep -q '^</s/l1>$'
check "six scripts in a chain is ELOOP, and nothing runs" $?
between K4 K4B | grep -qxF '</s/l1>' && between K4 K4B | grep -qxF '</s/l3>' \
    && between K4 K4B | grep -qxF '<z>'
check "four scripts in a chain run: plain l1 l2 l3 z" $?
hasre 'K5-END [1-9][0-9]*' && ! between K4B K5 | grep -q '^</s/noexec>$'
check "a script without execute permission is refused" $?
hasre 'K6-END [1-9][0-9]*'
check "an interpreter that does not exist is an error" $?
has 'SHELL-RAN-EMPTY'
check "#! naming nothing is ENOEXEC, and the shell runs the file, as on Linux" $?
has 'HASH-ONLY-RAN'
check "a '#' script with no '!' is still run by the shell" $?
owner() { debugfs -R "stat $1" "$DISK?offset=$OFF" 2>/dev/null \
            | sed -n 's/.*User: *\([0-9]*\).*/\1/p'; }
# Each owner into a variable FIRST: a $(...) in check's label runs after
# the test and before $? is read, and resets it to 0 -- which made these
# three checks pass on a kernel that never ran the interpreter at all.
o_elf=$(owner /w/by-elf); o_script=$(owner /w/by-script)
o_bash=$(owner /w/by-script-bash)
[ "$o_elf" = 1000 ]
check "control: a set-user-id ELF owned by 1000 runs as its owner ($o_elf)" $?
[ "$o_script" = 0 ]
check "a set-user-id SCRIPT does not: its interpreter runs as root ($o_script)" $?

echo "=== checks: bash (fork and execve) ==="
between K10 B1 | grep -qxF '</s/one> a|' && between K10 B1 | grep -qxF '<b c> |'
check "execve of a script runs its interpreter, with the same argv" $?
between B1 B2 | grep -qxF '</s/nest>' && between B1 B2 | grep -qxF '<y>'
check "and of a script whose interpreter is a script" $?
hasre 'B3-END [1-9][0-9]*' && ! between B2 B3 | grep -qxF '</s/l1>'
check "under execve too, six scripts is an error and nothing runs" $?
hasre 'B4-END [1-9][0-9]*' && ! grep -q 'SHELL-RAN-NOINT' "$CLEAN"
check "a missing interpreter is an error, and bash does not run the body" $?
[ "$o_bash" = 0 ]
check "set-user-id on a script is ignored under execve too ($o_bash)" $?
has '</s/plain>' && has '<e1>' && has '<e2>'
check "bash's exec replaces itself with the script's interpreter" $?
hasre 'B6-END 0'
check "  and the kernel shell is back afterwards" $?

grep -qE 'panic|bus error|address error|DOUBLE' "$CLEAN"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
