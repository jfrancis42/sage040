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

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

DISK=hd-edit.img
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG=edittest.log
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
make -s -C ../user || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" ../user/cube ::/CUBE
mcopy -o -i "$MIMG" ../user/spin ::/SPIN
mcopy -o -i "$MIMG" ../user/hello ::/HELLO
mcopy -o -i "$MIMG" ../user/shutdown ::/SHUTDOWN

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
: > session.tmp
{
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

    # --- & queues a job and says why it cannot run it ---
    printf 'hello queued-by-ampersand &\r'
    printf 'jobs\r'
    printf 'fg\r'

    # --- and the machine stops itself ---
    printf 'echo ok-about-to-shut-down\r'
    printf 'shutdown\r'
} >> session.tmp

rm -f in.fifo
mkfifo in.fifo

#
# No -no-reboot here would make `shutdown` restart the machine instead of
# ending it, which is the whole thing being tested.
#
"$QEMU" -M sage040 -cpu m68040 -m 4 \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < in.fifo > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> in.fifo
sleep "$BOOT_WAIT"
cat session.tmp >&3

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
rm -f in.fifo

#
# The terminal sends CR LF, so every line in the log ends with a stray
# carriage return. Strip it once here rather than allowing for it in
# twenty grep patterns.
#
tr -d '\r' < "$LOG" > clean.tmp

echo "=== guest session ==="
sed 's/^/  | /' clean.tmp

echo "=== checks: editing ==="

contains clean.tmp "kernel ready."
check "kernel reached its shell" $?

contains clean.tmp "ok-ctrl-a"
check "ctrl-A moved to the start of the line" $?

contains clean.tmp "ok-ctrl-b-XXXX"
check "ctrl-B moved left and inserted in the middle" $?

contains clean.tmp "ok-ctrl-u"
check "ctrl-U discarded everything before the cursor" $?

#
# From here the check is on a WHOLE OUTPUT LINE, not a substring. The
# editor echoes every keystroke, so the text that ctrl-W deleted really
# is in the log -- on the line showing what was typed. What matters is
# what `echo` then printed, which is a line of its own.
#
ran() {             # ran <exact output line>
    grep -qx -- "$1" clean.tmp
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

test "$(grep -c -- 'ok-history-source' clean.tmp)" -ge 3
check "ctrl-P recalled the previous line and ran it again" $?

contains clean.tmp "ok-hist-two"
check "ctrl-P walked back two lines and ctrl-N forward one" $?

test "$(grep -c -- 'ok-needle-found' clean.tmp)" -ge 3
check "ctrl-R found a line by substring and ran it" $?

contains clean.tmp "reverse-i-search"
check "  and showed the search prompt while it did" $?

echo "=== checks: ctrl-C ==="

contains clean.tmp "ok-after-ctrl-c"
check "the shell carried on after ctrl-C" $?

# Echoed as it was typed, then abandoned -- so the test is that `echo`
# never printed it on a line of its own.
! grep -qx -- "this must never run" clean.tmp
check "the abandoned line was echoed but never run" $?

grep -qF -- "^C" clean.tmp
check "  and the shell showed ^C the way a shell does" $?

contains clean.tmp "ok-bare-spin-interrupted"
check "ctrl-C ended a program making NO system calls -- the timer noticed" $?

contains clean.tmp "ok-calling-spin-interrupted"
check "ctrl-C ended one that was making them, at the boundary" $?

echo "=== checks: jobs ==="

contains clean.tmp "[1]+  stopped"
check "ctrl-Z stopped the running program" $?

contains clean.tmp "ok-stopped-and-listed"
check "the shell was usable while a job was stopped" $?

contains clean.tmp "ok-resumed-then-killed"
check "fg resumed the stopped job and it was still killable" $?

# A program resumed into a clobbered context dies on its own rather than
# running until it is killed, so a fault here means fg is lying.
! grep -q "exception" clean.tmp
check "  and nothing faulted doing it" $?

contains clean.tmp "queued -- nothing runs in the background"
check "& queued a job rather than pretending to run it" $?

contains clean.tmp "argv[1] = queued-by-ampersand"
check "fg ran the job that & had queued" $?

echo "=== checks: shutdown ==="

contains clean.tmp "ok-about-to-shut-down"
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
