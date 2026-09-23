#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# apitest.sh - the system call surface a ported program expects.
#
# The other suites test subsystems: the filesystem, memory protection,
# the editor, the network. This one tests the ABI itself -- the calls
# that exist so that software written for a Unix will build and run
# here, and that are individually dull and collectively the difference
# between a machine that can host other people's programs and one that
# cannot.
#
# It grows as that surface does. Each group below is a task from
# progress.md, and a group is only added once its calls actually work,
# so a failure here is always a regression rather than a thing not
# written yet.
#
# The checks are made by PROGRAMS in apps/, not by the shell, because
# the point is that a program can do these things. The program reports
# `ok` or `FAIL` per line and this script counts them -- so a new check
# is a line of C rather than a line of shell.

set -u

cd "$(dirname "$0")"

# How big the machine is. One place, shared with the Makefiles.
. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-api.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/apitest.log"
# Gone before QEMU starts, so a run that never reaches the guest has no
# log to grade -- rather than silently grading the last run's.
rm -f "$LOG"
BOOT_WAIT=${BOOT_WAIT:-4}

pass=0
fail=0

check() {
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../apps || exit 1
make -s -C ../system || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040

fsimg put kernel.rom /KERNEL.ROM
fsimg put -m 755 ../apps/statfs /statfs
fsimg put -m 755 ../apps/cdtest /cdtest
fsimg put -m 755 ../apps/hello /hello
fsimg put -m 755 ../apps/memtest /memtest
fsimg put -m 755 ../apps/malloctest /malloctest
fsimg put -m 755 ../apps/sigtest /sigtest
fsimg put -m 755 ../apps/fptest /fptest
fsimg put -m 755 ../apps/polltest /polltest
fsimg put -m 755 ../apps/timetest /timetest
fsimg put -m 755 ../apps/pipetest /pipetest
fsimg put -m 755 ../apps/proctest /proctest
fsimg put -m 755 ../apps/socktest /socktest
fsimg put -m 755 ../apps/spin /spin
fsimg mkdir /ETC
fsimg mkdir /bin
fsimg put -m 755 ../system/env /bin/env
fsimg put -m 755 ../system/sh /bin/sh
fsimg put -m 755 ../system/ping /bin/ping
printf 'echo from-a-script\r\nexit 6\r\n' > "$SCRATCH/t.tmp"
fsimg put "$SCRATCH/t.tmp" /T.SH
printf 'echo rc-ran\r\n' > "$SCRATCH/rc.tmp"
fsimg put "$SCRATCH/rc.tmp" /ETC/RC

# PATH: the same program name in two directories, to see which is
# found. /bin/hello and /OTHER/hello are different programs -- one
# prints "hello from a program", the other is `env`, which prints the
# environment -- so which one ran is visible in the output.
fsimg mkdir /OTHER
fsimg put -m 755 ../apps/hello /bin/hello2
fsimg put -m 755 ../system/env /OTHER/hello2

: > "$SCRATCH/session.tmp"
{
    # --- descriptors: fstat, access, dup, isatty ---
    printf 'statfs /ETC/RC\r';          sleep 2

    # --- the working directory belongs to the task ---
    printf 'pwd\r';                     sleep 1
    printf 'cdtest /bin\r';             sleep 2
    printf 'pwd\r';                     sleep 1

    # --- an absolute path means the same thing from anywhere ---
    printf 'cd /ETC\r';                 sleep 1
    printf 'cat /ETC/RC\r';             sleep 1
    printf 'stat /bin/env\r';           sleep 1
    printf '/bin/env\r';                sleep 1
    printf 'cd /\r';                    sleep 1

    # --- PATH: what a bare name means ---
    printf 'echo $PATH\r';                         sleep 1
    printf 'hello2\r';                             sleep 2
    printf 'export PATH=/OTHER:/bin\r';            sleep 1
    printf 'hello2\r';                             sleep 2
    printf 'export PATH=/bin:.\r';                 sleep 1
    printf 'cd /OTHER\r';                          sleep 1
    printf 'hello2\r';                             sleep 2
    printf './hello2\r';                           sleep 2
    printf 'cd /\r';                               sleep 1
    printf 'export PATH=/NOSUCHDIR\r';             sleep 1
    printf 'hello2\r';                             sleep 2
    printf 'export PATH=/bin:.\r';                 sleep 1
    printf 'echo PATH-DONE\r';                     sleep 1

    # --- the gate, and signals ---
    printf 'sigtest\r';                 sleep 6
    printf 'polltest\r';                sleep 4
    printf 'timetest\r';                sleep 8
    printf 'timetest alarm\r';          sleep 2

    # --- pipes, redirection, pipelines ---
    printf 'pipetest\r';                            sleep 3
    printf 'hello > /OUT.TXT\r';                    sleep 1
    printf 'hello again >> /OUT.TXT\r';             sleep 1
    printf 'pipetest out redirected-line >/RED.TXT\r'; sleep 1
    printf 'pipetest err to-stderr 2> /ERR.TXT\r';  sleep 1
    printf 'pipetest err both 2>&1 > /BOTH.TXT\r';  sleep 1
    printf 'cat /ETC/RC > /CAT.TXT\r';              sleep 1
    printf 'pipetest cat < /ETC/RC\r';              sleep 1
    printf 'pipetest out hi | pipetest count\r';    sleep 2
    printf 'cat /ETC/RC | pipetest count\r';        sleep 2
    printf 'pipetest gen 100000 | pipetest cat | pipetest check 100000\r'; sleep 6
    printf 'pipetest out x | nosuchcmd\r';          sleep 2
    printf 'pipetest gen 100000 | pipetest out reader-gone\r'; sleep 2
    printf 'echo AFTER-PIPELINES\r';                sleep 1

    # --- sockets, over loopback and socket pairs ---
    printf 'socktest\r';                            sleep 8
    printf 'ping 127.0.0.1 2\r';                    sleep 3

    # --- fork, execve, waitpid, and /bin/sh ---
    printf 'proctest\r';                            sleep 7
    printf "sh -c 'pipetest out via-sh-c | pipetest count'\r"; sleep 2
    printf "sh -c 'exit 3'\r";                      sleep 1
    printf 'echo SH-C-STATUS=$?\r';                 sleep 1
    printf 'sh /T.SH\r';                            sleep 1
    printf 'echo SH-FILE-STATUS=$?\r';              sleep 1
    printf "echo 'a  |  b' \"x > y\" c\\\\ d\r";          sleep 1
    printf 'export QV=quoted-value\r';              sleep 0.5
    printf "echo '\$QV' \"\$QV\"\r";                sleep 1
    printf 'nosuchcmd\r';                           sleep 1
    printf 'echo NOTFOUND-STATUS=$?\r';             sleep 1
    printf 'sh\r';                                  sleep 1.5
    printf 'hello inside-bin-sh\r';                 sleep 1.5
    printf 'exit 4\r';                              sleep 1
    printf 'echo SH-EXIT-STATUS=$?\r';              sleep 1
    printf 'sigtest badstack\r';        sleep 1.5
    printf 'sigtest forge\r';           sleep 1.5
    printf 'kill 2\r';                  sleep 1
    printf 'kill -9 2\r';               sleep 1

    # --- memory: brk, sbrk, mmap, munmap, mprotect ---
    printf 'echo FREE-BEFORE\r';        sleep 0.5
    printf 'free\r';                    sleep 0.5
    printf 'memtest\r';                 sleep 4
    printf 'echo FREE-AFTER\r';         sleep 0.5
    printf 'free\r';                    sleep 0.5
    printf 'memtest past\r';            sleep 2
    printf 'memtest unmapped\r';        sleep 2
    printf 'memtest readonly\r';        sleep 2
    printf 'memtest none\r';            sleep 2

    # --- the allocator: its workload runs as long as the host takes ---
    printf 'mallocte\r'
} >> "$SCRATCH/session.tmp"

rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"
cat "$SCRATCH/session.tmp" >&3

# Waited for rather than slept past: how long 40000 allocations take is
# a property of the host, and a sleep sized on a fast one fails on a slow
# one. Anything typed meanwhile would go to malloctest, not the shell.
for _ in $(seq 1 600); do
    if grep -qF "malloctest: done" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
    sleep 0.2
done
sleep 0.5
{
    # Stop a job with SIGSTOP, then kill it while it is stopped.
    printf 'spin &\r';                  sleep 1
    printf 'ps\r';                      sleep 1
} >&3
# Send once `text` is in the log -- for keys that must arrive while a
# particular program is running, which a pre-built session cannot time.
send_after() {
    for _ in $(seq 1 100); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then break; fi
        sleep 0.2
    done
    sleep 0.3
    printf '%b' "$2" >&3
}
printf 'sigtest catchint\r' >&3
send_after "sigtest: waiting in pause for ctrl-C" '\003'
sleep 1
printf 'sigtest spincatch\r' >&3
send_after "sigtest: computing until ctrl-C" '\003'
sleep 1
# ctrl-C reaches every stage of a pipeline: they share a group.
printf 'pipetest sleepy | pipetest sleepy\r' >&3
send_after "pipetest: sleepy" '\003'
sleep 1
printf 'echo PS-AFTER-PIPE-INTERRUPT\r' >&3
sleep 0.5
printf 'ps\r' >&3
sleep 1
# A background reader is stopped by SIGTTIN, not given the keys; after
# fg, it reads as if nothing had happened.
printf 'pipetest readtty &\r' >&3
sleep 1.5
printf 'echo JOBS-TTIN\r' >&3
sleep 0.5
printf 'jobs\r' >&3
sleep 1
printf 'fg\r' >&3
sleep 1.5
printf 'typed-after-fg\r' >&3
send_after "pipetest: read typed-after-fg" ''
sleep 0.5
printf 'polltest tty\r' >&3
send_after "polltest: press a key for poll" 'k\r'
send_after "polltest: press a key for select" 'x\r'
send_after "polltest: tty done" ''
sleep 0.5

spin_pid=
for _ in $(seq 1 50); do
    spin_pid=$(tr -d '\r' < "$LOG" |
               awk '$1 ~ /^[0-9]+$/ && $NF == "spin" { p = $1 } END { print p }')
    [ -n "$spin_pid" ] && break
    sleep 0.2
done
{
    printf 'kill -19 %s\r' "${spin_pid:-0}";  sleep 1
    printf 'echo PS-STOPPED\r';              sleep 0.5
    printf 'ps\r';                           sleep 1
    printf 'kill -9 %s\r' "${spin_pid:-0}";   sleep 1
    printf '\r';                             sleep 0.5
    printf 'echo PS-KILLED\r';               sleep 0.5
    printf 'ps\r';                           sleep 1
    printf 'mallocte doublefree\r';     sleep 2
    # Two programs using the FPU at once, with different values in every
    # register: one in the background, one in the foreground.
    printf 'fptest 1 &\r';              sleep 0.5
    printf 'fptest 2\r';                sleep 5
    printf 'echo SHELL-SURVIVED\r'
} >&3

for _ in $(seq 1 200); do
    if grep -qF "SHELL-SURVIVED" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
    sleep 0.2
done

sleep 0.5
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"
C="$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$C"

# ---------------------------------------------------------------
# The program's own checks. Each `ok` line it printed is a pass and
# each `FAIL` line is a failure, reported here under its own name so
# that a break says which call stopped working.
# ---------------------------------------------------------------
echo "=== checks: the programs' own (descriptors, brk, mmap, mprotect) ==="

grep -q "statfs: done" "$C"
check "statfs ran to the end" $?

grep -q "memtest: done" "$C"
check "memtest ran to the end" $?

grep -q "malloctest: done" "$C"
check "malloctest ran to the end" $?

grep -q "sigtest: done" "$C"
check "sigtest ran to the end" $?

grep -q "polltest: done" "$C"
check "polltest ran to the end" $?

grep -q "timetest: done" "$C"
check "timetest ran to the end" $?

grep -q "pipetest: done" "$C"
check "pipetest ran to the end" $?

grep -q "socktest: done" "$C"
check "socktest ran to the end" $?
grep -q "2 sent, 2 received" "$C"
check "ping 127.0.0.1 is answered, with no network configured" $?

grep -q "proctest: done" "$C"
check "proctest ran to the end" $?
grep -q "proctest: exec'd with argument-one and PROCVAR=from-execve" "$C"
check "  and the program it exec'd said what it was given" $?

grep -q "polltest: tty done" "$C"
check "polltest waited for keys with poll and select" $?

test "$(grep -c '^  FAIL ' "$C")" -eq 0
check "  and every check inside them passed" $?

# Named individually, so a regression says which one.
while IFS= read -r line; do
    what=${line#  ok   }
    what=${what#  FAIL }
    case "$line" in
        "  ok   "*)   check "$what" 0 ;;
        "  FAIL "*)   check "$what" 1 ;;
    esac
done < <(grep -E '^  (ok|FAIL) ' "$C")

echo "=== checks: the working directory belongs to the task ==="

grep -q "cdtest: now in /bin" "$C"
check "a program can chdir itself somewhere" $?

# The shell printed pwd twice, before and after. Both must say "/".
test "$(grep -c '^/$' "$C")" -ge 2
check "  and the shell that started it did NOT move" $?

echo "=== checks: an absolute path is absolute ==="

grep -q "rc-ran" "$C"
check "/etc/rc ran at startup" $?

# cat /ETC/RC issued from inside /ETC. vfs.c used to strip the leading
# slash, so it resolved relative to the cwd and became /ETC/ETC/RC.
test "$(grep -c 'echo rc-ran' "$C")" -ge 1
check "cat of an absolute path worked from inside that directory" $?

grep -q "PATH=" "$C"
check "a program ran by absolute path from another directory" $?

echo "=== checks: PATH ==="

# /usr/bin is between them: /bin first so the system's own programs win
# a name clash, then /usr/bin where a package installed with
# --prefix=/usr puts itself -- the native toolchain -- and "." last, so
# a program dropped in the working directory cannot stand in for either.
grep -q "^/bin:/usr/bin:\.$" "$C"
check "the shell sets PATH to /bin:/usr/bin:. before /etc/rc runs" $?

# Between "hello2" the first time and the export, the /bin one ran.
sed -n '/echo \$PATH/,/export PATH=\/OTHER/p' "$C" | grep -q "hello from a program"
check "a bare name is found along PATH" $?

# With /OTHER first, the OTHER one runs -- which is `env`, and prints
# the environment rather than a greeting.
sed -n '/export PATH=\/OTHER/,/export PATH=\/bin:\.$/p' "$C" | grep -q "PATH=/OTHER:/bin"
check "  and PATH is an order of preference, not a set" $?

sed -n '/export PATH=\/OTHER/,/export PATH=\/bin:\.$/p' "$C" |
    grep -qv "hello from a program"
check "  the earlier directory won" $?

# Back to /bin:. and standing in /OTHER: the bare name finds /bin's,
# because . is last; ./hello2 finds the one here, because a name with a
# slash is a path and not a search.
sed -n '/^\/OTHER\$ hello2/,/^\/OTHER\$ \.\/hello2/p' "$C" | grep -q "hello from a program"
check "the current directory is searched LAST, not first" $?

sed -n '/^\/OTHER\$ \.\/hello2/,/^\/\$ export PATH=\/NOSUCHDIR/p' "$C" |
    grep -q "PATH=/bin:\."
check "  and ./name runs the one here whatever PATH says" $?

sed -n '/export PATH=\/NOSUCHDIR/,/PATH-DONE/p' "$C" | grep -q "not found"
check "a name that is on no directory of PATH is not found" $?

grep -q "PATH-DONE" "$C"
check "  and the shell carried on afterwards" $?

echo "=== checks: memory the heap gave back is gone ==="

grep -q "memtest: touching memory the heap gave back" "$C"
check "memtest shrank its heap and reached past the end" $?

! grep -q "NOT-PROTECTED" "$C"
check "  and the access was refused" $?

test "$(grep -c "^memtest: segmentation fault" "$C")" -eq 4
check "  and the program was killed for it" $?

for mode in unmapped readonly none; do
    grep -q "memtest: abusing a $mode page" "$C"
    check "memtest touched a $mode page" $?
done
check "  and all four were killed, none got through" \
    "$(grep -c "NOT-PROTECTED" "$C")"
grep -q "memtest: read-only page still reads" "$C"
check "a read-only page was readable up to the write" $?

echo "=== checks: exit gives back every page, mapped or not ==="

used_after() {
    awk -v m="$1" '$0 == m { f = 1 } f && $1 == "used" { print $2; exit }' "$C"
}
before=$(used_after FREE-BEFORE)
after=$(used_after FREE-AFTER)
echo "  used pages: before=${before:-?} after=${after:-?}"
test -n "$before" && test -n "$after" && [ "$before" -eq "$after" ]
check "memtest left mappings, PROT_NONE pages and a file behind, and none leaked" $?

echo "=== checks: the allocator catches a double free ==="

grep -q "malloctest: freeing the same block twice" "$C"
check "malloctest freed a block twice" $?

grep -q "free(): invalid pointer or double free at 0x" "$C"
check "  and free() said so" $?

! grep -q "NOT-CAUGHT" "$C"
check "  and the program did not carry on" $?

echo "=== checks: every task has its own FPU ==="

grep -q "fptest 1: fpu state kept" "$C"
check "a background task kept its FP registers and rounding mode" $?
grep -q "fptest 2: fpu state kept" "$C"
check "  and so did the foreground task running beside it" $?
! grep -q "FPU STATE LOST" "$C"
check "  and neither saw the other's" $?

echo "=== checks: signals reach handlers from everywhere ==="

grep -q "sigtest: nap slept its full time" "$C"
check "a signal to one sleeping task does not wake the others" $?

grep -q "sigtest: caught SIGINT in pause" "$C"
check "ctrl-C reached a handler in a program waiting in pause()" $?

grep -q "sigtest: caught SIGINT while computing" "$C"
check "ctrl-C reached a handler in a program making no system calls" $?

grep -q "sigtest: taking a signal with an unusable stack" "$C" &&
    ! grep -q "SIGNAL DELIVERED ONTO NOTHING" "$C"
check "a signal that cannot be given a frame does not return" $?

# Proved by the privilege violation, not by the absence of a message:
# a forgery that worked could take the machine down before printing.
grep -A3 "sigtest: sigreturn to a context that asks for supervisor" "$C" |
    grep -q "privilege violation" && ! grep -q "SUPERVISOR MODE REACHED" "$C"
check "a forged sigreturn cannot reach supervisor mode" $?

test "$(grep -c "^sigtest: segmentation fault" "$C")" -eq 2
check "  and both of those programs were killed for it" $?

echo "=== checks: an alarm nobody catches ends the program ==="

grep -q "timetest: waiting for an alarm nobody catches" "$C" &&
    grep -q "^timetest: alarm clock" "$C" &&
    ! grep -q "ALARM DID NOT END THE PROGRAM" "$C"
check "SIGALRM's default action is to terminate, and the shell says why" $?

echo "=== checks: /bin/sh, and quoting ==="

grep -q "pipetest: counted 9 bytes" "$C"
check "sh -c ran a pipeline" $?
grep -q "^SH-C-STATUS=3" "$C"
check "  and sh -c's exit status reached \$?" $?
grep -q "^from-a-script" "$C" && grep -q "^SH-FILE-STATUS=6" "$C"
check "sh FILE ran a script, and its exit status came back" $?
grep -q "^a  |  b x > y c d$" "$C"
check "quotes keep spaces, | and > literal, and a backslash quotes" $?
grep -q '^\$QV quoted-value$' "$C"
check "single quotes stop \$ expansion; double quotes do not" $?
grep -q "^NOTFOUND-STATUS=127" "$C"
check "a command that is not found is status 127" $?
grep -q "argv\[1\] = inside-bin-sh" "$C" && grep -q "^SH-EXIT-STATUS=4" "$C"
check "an interactive /bin/sh ran a program, and exit 4 came back" $?

echo "=== checks: redirection, read back on the host ==="

# Read from the image after the machine has stopped: the kernel's writes
# are checked by something other than the code that made them.
host_file() {
    fsimg cat /$1 2>/dev/null | tr -d '\r'
}
[ "$(host_file OUT.TXT | grep -c 'hello from a program')" -eq 2 ]
check "a program's output went to a file with >, and >> appended" $?
[ "$(host_file RED.TXT)" = "redirected-line" ]
check "  and >FILE with no space works" $?
[ "$(host_file ERR.TXT)" = "to-stderr" ]
check "2> caught what a program wrote to stderr" $?
[ "$(host_file BOTH.TXT)" = "both" ]
check "2>&1 sent stderr where stdout went" $?
[ "$(host_file CAT.TXT)" = "$(host_file ETC/RC)" ]
check "a builtin's output was redirected" $?
grep -q "^echo rc-ran" "$C"
check "< fed a file to a program's stdin" $?

echo "=== checks: pipelines ==="

grep -q "pipetest: counted 3 bytes" "$C"
check "program | program" $?
grep -q "pipetest: counted $(fsimg cat /ETC/RC | wc -c) bytes" "$C"
check "builtin | program" $?
grep -q "pipetest: check passed, 100000 bytes" "$C"
check "100000 bytes through three stages, every one in order" $?
grep -q "^nosuchcmd: command not found" "$C"
check "a stage that does not exist is reported" $?
grep -q "^reader-gone" "$C" && grep -q "^AFTER-PIPELINES" "$C"
check "a writer whose reader has gone does not hang the pipeline" $?

awk '/^PS-AFTER-PIPE-INTERRUPT$/ { f = 1 } f && /pipetest sleepy/ { bad = 1 }
     f && /^\/\$ pipetest readtty/ { exit } END { exit bad }' "$C"
check "ctrl-C ended both stages of a pipeline" $?

awk '/^JOBS-TTIN$/ { f = 1 } f && /stop/ && /pipetest readtty/ { ok = 1 }
     f && /^\/\$ fg/ { exit } END { exit !ok }' "$C"
check "a background reader was stopped instead of taking the keys" $?
grep -q "pipetest: read typed-after-fg" "$C"
check "  and after fg it read the line typed to it" $?

echo "=== checks: stopping, and killing what is stopped ==="

# The line for spin in the ps listing after each marker.
spin_state() {
    awk -v m="$1" -v p="$spin_pid" '$0 == m { f = 1 }
        f && $1 == p { print $3; exit }
        f && /PS-/ && $0 != m { exit }' "$C"
}
test -n "$spin_pid" && [ "$(spin_state PS-STOPPED)" = "stop" ]
check "kill -19 (SIGSTOP) stopped a job" $?
test -n "$spin_pid" && [ -z "$(spin_state PS-KILLED)" ]
check "  and kill -9 ended it while it was stopped" $?

echo "=== checks: the kernel's own tasks take no signals ==="

# The shell is pid 2 and a kernel task. A signal to it could never be
# acted on, so kill refuses rather than leaving one pending for ever.
test "$(grep -c "^kill: operation not permitted" "$C")" -eq 2
check "kill and kill -9 of the shell are refused with EPERM" $?

echo "=== checks: nothing broke ==="

grep -q "SHELL-SURVIVED" "$C"
check "the shell survived all of it" $?

! grep -qE "exception|panic|DOUBLE" "$C"
check "no faults anywhere" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
[ "$fail" -eq 0 ]
