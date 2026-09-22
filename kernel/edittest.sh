#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# edittest.sh - drive the line editor, the history and job control from
# the console, and check what came back.
#
# This works at all because the editor does not care where a keystroke
# came from. It puts the terminal in raw mode and reads bytes, so a
# control character sent down the serial line is indistinguishable from
# one typed on the keyboard -- which means ctrl-A, ctrl-R and ctrl-C can
# be tested by writing 0x01, 0x12 and 0x03 into a FIFO, with no keyboard
# and no display involved.
#
# The check is always "what did the shell end up running", never "what
# did the screen look like". Cursor movement is done with backspaces and
# re-echoed characters, so the log contains every intermediate state of
# the line; asserting on that would be asserting on the redraw strategy
# rather than on the editing, and it would break the first time the
# redrawing got smarter. Asserting on the command that ran does not.
#
# Runs on a scratch image, so the machine's own disk is left alone.

set -u

cd "$(dirname "$0")"

# How big the machine is. One place, shared with the Makefiles.
. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

#
# Everything this test writes goes in one place.
#
# Scratch disk images are 16 MB each and there is one per test suite, so
# leaving them beside the source meant 67 MB of build product scattered
# through the tree with names that looked like part of it. They are all
# under scratch/ now, which `make clean` removes and git ignores.
#
#
# Computed AFTER the cd above, from the working directory rather than
# from $0 -- which has already been used once and is relative to where
# the script was invoked from, not to where it now is. Deriving it from
# $0 a second time worked when the script was run as ./edittest.sh and
# failed when it was run by path, which is a difference nobody should
# have to notice.
#
SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-edit.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/edittest.log"
# Gone before QEMU starts, so a run that never reaches the guest has no
# log to grade -- rather than silently grading the last run's.
rm -f "$LOG"
BOOT_WAIT=${BOOT_WAIT:-4}

pass=0
fail=0

check() {           # check <description> <condition-exit-status>
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $1"
        fail=$((fail + 1))
    fi
}

contains() {        # contains <file> <text>
    grep -qF -- "$2" "$1"
}

absent() {          # absent <file> <text>
    ! grep -qF -- "$2" "$1"
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../apps || exit 1
make -s -C ../system || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" ../apps/cube ::/CUBE
mcopy -o -i "$MIMG" ../apps/spin ::/SPIN
mcopy -o -i "$MIMG" ../apps/hello ::/HELLO
mcopy -o -i "$MIMG" ../system/shutdown ::/SHUTDOWN
mcopy -o -i "$MIMG" ../apps/napper ::/NAPPER

#
# The session.
#
# Control characters go in as escapes and printf turns them into bytes.
# Each line ends with \r rather than \n because that is what a terminal
# sends when Return is pressed, and the kernel's ICRNL turns it into a
# newline -- exercising the same path a person does.
#
#   \001 ctrl-A   \002 ctrl-B   \005 ctrl-E   \006 ctrl-F
#   \003 ctrl-C   \013 ctrl-K   \016 ctrl-N   \020 ctrl-P
#   \022 ctrl-R   \023 ctrl-S   \025 ctrl-U   \027 ctrl-W
#   \032 ctrl-Z   \033 escape
#
#
# THE SLEEPS HAVE TO HAPPEN WHILE SENDING, not while building.
#
# This was a block that wrote to a file, so every sleep in it delayed the
# construction of the file and none of them delayed the guest -- the
# whole session then arrived in one burst. It did not matter until
# signals became precise: a ctrl-Z typed "two seconds after" a command
# actually landed before that command's program existed, went to the
# shell, and was ignored. A person typing at a terminal does not do
# that, so neither should this.
#
feed() {
    # --- ctrl-A: go to the start and type in front of what is there ---
    printf 'ok-ctrl-a\001echo \r'

    # --- ctrl-E and ctrl-B: back over four, insert, then to the end ---
    printf 'echo ok-XXXX\002\002\002\002ctrl-b-\005\r'

    # --- ctrl-U: throw away everything typed so far ---
    printf 'this line is rubbish\025echo ok-ctrl-u\r'

    # --- ctrl-W: delete the word before the cursor ---
    printf 'echo ok-ctrl-w JUNKWORD\027\r'

    # --- ctrl-K: delete from the cursor to the end ---
    printf 'echo ok-ctrl-k TAIL\002\002\002\002\013\r'

    # --- backspace and DEL both erase backwards: two of each, four
    #     characters to remove ---
    printf 'echo ok-bsXYZW\010\010\177\177\r'

    # --- arrow keys: left four, insert, right to the end ---
    printf 'echo ok-XXXX\033[D\033[D\033[D\033[D-arrow-\033[C\033[C\r'

    # --- Home and End ---
    printf 'ok-home\033[Hecho \033[F\r'

    # --- ctrl-P: run the previous line again ---
    printf 'echo ok-history-source\r'
    printf '\020\r'

    # --- ctrl-P twice then ctrl-N: back two, forward one ---
    printf 'echo ok-hist-two\r'
    printf 'echo ok-hist-one\r'
    printf '\020\020\016\r'

    # --- ctrl-R: search the history backwards for a substring ---
    printf 'echo ok-needle-found\r'
    printf 'echo something else\r'
    printf '\022needle\r'

    # --- ctrl-C on a half-typed line abandons it and prompts again ---
    printf 'echo this must never run\003'
    printf 'echo ok-after-ctrl-c\r'

    # --- the history builtin ---
    printf 'history\r'

    # --- ctrl-C on a program that will not stop on its own ---
    #
    # `spin` and not `cube`. cube polls the keyboard and exits on any
    # key, so ctrl-C appears to work on it whether or not signals do
    # anything at all -- which is exactly what this test used to be
    # fooled by. spin never reads and never exits, so the only way out
    # of it is the kernel taking it away.
    #
    # The bare form makes no system calls whatsoever, so nothing but the
    # timer interrupt can notice the keystroke.
    printf 'spin\r'
    sleep 2
    printf '\003'
    sleep 2
    printf 'echo ok-bare-spin-interrupted\r'

    # And the form that does make system calls, where the boundary path
    # is what notices.
    printf 'spin calls\r'
    sleep 2
    printf '\003'
    sleep 2
    printf 'echo ok-calling-spin-interrupted\r'

    # --- ctrl-Z stops a program, jobs lists it, fg resumes and ctrl-C
    #     ends it. A program can only be stopped at a system call
    #     boundary, so this is the calling form. ---
    printf 'spin calls\r'
    sleep 2
    printf '\032'
    sleep 2
    printf 'jobs\r'
    printf 'echo ok-stopped-and-listed\r'
    printf 'fg\r'
    sleep 2
    printf '\003'
    sleep 2
    printf 'echo ok-resumed-then-killed\r'
    sleep 1

    # --- & runs a job alongside the shell, which is the point of it ---
    #
    # `spin calls` never ends on its own, so if the prompt comes back and
    # another command runs, the two were genuinely running at once.
    printf 'spin calls &\r'
    sleep 2
    printf 'echo ok-shell-alive-with-background\r'
    sleep 2
    printf 'jobs\r'
    sleep 2
    printf 'hello alongside\r'
    sleep 3

    # --- a sleeping task must not stop the machine ---
    #
    # nanosleep() used to loop on STOP in supervisor mode. Preemption
    # only happens on the way back to user mode, so a task in that loop
    # was never preempted and NOTHING else ran for the length of the
    # sleep: `napper 6 &` froze the shell for six seconds. It has to
    # sleep on a wait queue instead, and the proof is that work issued
    # afterwards finishes BEFORE the sleeper wakes.
    printf 'napper 5 &\r'
    sleep 2
    printf 'echo ok-ran-while-one-task-slept\r'
    sleep 6

    # --- and the machine stops itself ---
    printf 'echo ok-about-to-shut-down\r'
    printf 'shutdown\r'
}

rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"

#
# No -no-reboot here would make `shutdown` restart the machine instead of
# ending it, which is the whole thing being tested.
#
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"
feed >&3 &
feeder=$!

#
# The session contains sleeps, so it takes a while to feed. Wait for the
# emulator to go away on its own -- which is what `shutdown` is supposed
# to cause -- and only kill it if it does not.
#
exited=1
for _ in $(seq 1 400); do
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        exited=0
        break
    fi
    sleep 0.2
done

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo"

#
# The terminal sends CR LF, so every line in the log ends with a stray
# carriage return. Strip it once here rather than allowing for it in
# twenty grep patterns.
#
tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/clean.tmp"

echo "=== checks: editing ==="

contains "$SCRATCH/clean.tmp" "kernel ready."
check "kernel reached its shell" $?

contains "$SCRATCH/clean.tmp" "ok-ctrl-a"
check "ctrl-A moved to the start of the line" $?

contains "$SCRATCH/clean.tmp" "ok-ctrl-b-XXXX"
check "ctrl-B moved left and inserted in the middle" $?

contains "$SCRATCH/clean.tmp" "ok-ctrl-u"
check "ctrl-U discarded everything before the cursor" $?

#
# From here the check is on a WHOLE OUTPUT LINE, not a substring. The
# editor echoes every keystroke, so the text that ctrl-W deleted really
# is in the log -- on the line showing what was typed. What matters is
# what `echo` then printed, which is a line of its own.
#
ran() {             # ran <exact output line>
    grep -qx -- "$1" "$SCRATCH/clean.tmp"
}

ran "ok-ctrl-w"
check "ctrl-W deleted the word before the cursor" $?

ran "ok-ctrl-k"
check "ctrl-K deleted from the cursor to the end" $?

ran "ok-bs"
check "backspace and DEL both erased backwards" $?

ran "ok--arrow-XXXX"
check "the arrow keys moved left and right, inserting in the middle" $?

ran "ok-home"
check "Home and End moved to the ends of the line" $?

echo "=== checks: history ==="

test "$(grep -c -- 'ok-history-source' "$SCRATCH/clean.tmp")" -ge 3
check "ctrl-P recalled the previous line and ran it again" $?

contains "$SCRATCH/clean.tmp" "ok-hist-two"
check "ctrl-P walked back two lines and ctrl-N forward one" $?

test "$(grep -c -- 'ok-needle-found' "$SCRATCH/clean.tmp")" -ge 3
check "ctrl-R found a line by substring and ran it" $?

contains "$SCRATCH/clean.tmp" "reverse-i-search"
check "  and showed the search prompt while it did" $?

echo "=== checks: ctrl-C ==="

contains "$SCRATCH/clean.tmp" "ok-after-ctrl-c"
check "the shell carried on after ctrl-C" $?

# Echoed as it was typed, then abandoned -- so the test is that `echo`
# never printed it on a line of its own.
! grep -qx -- "this must never run" "$SCRATCH/clean.tmp"
check "the abandoned line was echoed but never run" $?

grep -qF -- "^C" "$SCRATCH/clean.tmp"
check "  and the shell showed ^C the way a shell does" $?

contains "$SCRATCH/clean.tmp" "ok-bare-spin-interrupted"
check "ctrl-C ended a program making NO system calls -- the timer noticed" $?

contains "$SCRATCH/clean.tmp" "ok-calling-spin-interrupted"
check "ctrl-C ended one that was making them, at the boundary" $?

echo "=== checks: jobs ==="

grep -qE '\]\+  stopped' "$SCRATCH/clean.tmp"
check "ctrl-Z stopped the running program" $?

contains "$SCRATCH/clean.tmp" "ok-stopped-and-listed"
check "the shell was usable while a job was stopped" $?

contains "$SCRATCH/clean.tmp" "ok-resumed-then-killed"
check "fg resumed the stopped job and it was still killable" $?

# A program resumed into a clobbered context dies on its own rather than
# running until it is killed, so a fault here means fg is lying.
! grep -q "exception" "$SCRATCH/clean.tmp"
check "  and nothing faulted doing it" $?

contains "$SCRATCH/clean.tmp" "ok-shell-alive-with-background"
check "& returned immediately and the shell kept working" $?

grep -qE '^\[[0-9]+\]  running  spin calls' "$SCRATCH/clean.tmp"
check "  and the background job really was running" $?

contains "$SCRATCH/clean.tmp" "argv[1] = alongside"
check "  a second program ran at the same time as it" $?

echo "=== checks: a sleeping task does not stop the machine ==="

contains "$SCRATCH/clean.tmp" "napper: sleeping"
check "a task went to sleep" $?

# The ORDER is the whole test: sleeping, then the echo, then awake. The
# echo is typed two seconds into a five second sleep, so if the sleeper
# is yielding then its output lands after the echo's. If nanosleep halts
# the processor instead, nothing runs until the sleep ends and the two
# come out the other way round.
awk '/ok-ran-while-one-task-slept/ { ran = NR }
     /napper: awake/            { woke = NR }
     END { exit !(ran && woke && ran < woke) }' "$SCRATCH/clean.tmp"
check "  other tasks ran while it slept, and finished first" $?

echo "=== checks: shutdown ==="

contains "$SCRATCH/clean.tmp" "ok-about-to-shut-down"
check "the session got as far as shutdown" $?

test "$exited" -eq 0
check "shutdown stopped the machine: the emulator exited by itself" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
