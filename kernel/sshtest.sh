#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# sshtest.sh - ssh, scp and rsync, against a real OpenSSH client.
#
# The machine runs Dropbear; the thing on the other end is the
# workstation's own OpenSSH, which knows nothing about this project.
# That is the point: the protocol is either right or it is not, and no
# code in this tree gets a vote.
#
# THE FIRST CHECK IS THE ONE THAT MATTERS, and it is here because of
# what it caught. TEN successive connections, not one. The machine
# served exactly one ssh session and then transmitted nothing ever
# again -- no data, no ACKs, not even a SYN-ACK for a new connection --
# and a suite that made one connection and called ssh working would
# have passed on a machine whose network died the moment it was used.
#
# The cause was the LAN91C111's transmit allocation. Asking the chip
# for a page is a REQUEST, not a question: when none is free it
# remembers and grants one as soon as a page is released. The driver
# used to give up on the timeout and ask again later, and the second
# request clears the first -- so the page granted to the abandoned
# request was never given back by anybody. The chip has four. Four
# abandoned grants and it can neither send nor receive.
#
# ping is here for the same reason it was useful in finding that:
# net_wait() pumps the stack from the CALLING task, so ping works even
# when netd does not, and a difference between the two says where to
# look.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
DBOUT=$SRCDIR/build-dropbear-sage040/sage040
RSOUT=$SRCDIR/build-rsync-sage040/sage040

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-ssh.img"
PART_LBA=2048
MIMG="$DISK@@$((PART_LBA * 512))"
LOG="$SCRATCH/sshtest.log"
WORK="$SCRATCH/ssh.tmp"
PORT=${SSH_TEST_PORT:-2242}
rm -f "$LOG"; rm -rf "$WORK"; mkdir -p "$WORK"
BOOT_WAIT=${BOOT_WAIT:-5}

pass=0
fail=0
check() {
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}

for t in ssh scp ssh-keygen rsync; do
    command -v $t > /dev/null || { echo "sshtest: no $t here"; echo "RESULT: SKIP"; exit 0; }
done

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh ifconfig ping netstat || exit 1
make -s -C ../ldso || exit 1
[ -x "$DBOUT/bin/dropbear" ] || ../ports/dropbear/build.sh > /dev/null || exit 1
[ -x "$RSOUT/bin/rsync" ]    || ../ports/rsync/build.sh    > /dev/null || exit 1
[ -x ../ports/sbase/bin/cat ] || ../ports/sbase/build.sh > /dev/null || exit 1

KEY=$WORK/id
ssh-keygen -t ed25519 -N '' -f "$KEY" -q

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (64 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mmd -i "$MIMG" ::/BIN ::/lib ::/etc ::/ST ::/root ::/root/.ssh
for p in sh ifconfig ping netstat; do
    mcopy -o -i "$MIMG" "../system/$p" "::/BIN/$p"
done
mcopy -o -i "$MIMG" ../ldso/ld.so ::/lib/ld.so
mcopy -o -i "$MIMG" "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" ::/lib/libc.so
mcopy -o -i "$MIMG" ../system/passwd ::/etc/passwd
mcopy -o -i "$MIMG" ../system/group ::/etc/group
for b in dropbear scp dropbearkey; do
    mcopy -o -i "$MIMG" "$DBOUT/bin/$b" "::/BIN/$b"
done
mcopy -o -i "$MIMG" "$RSOUT/bin/rsync" ::/BIN/rsync
# The whole of sbase: a missing `true` once made a readiness check
# report the machine unreachable when it was perfectly well.
for p in $(ls ../ports/sbase/bin); do
    mcopy -o -i "$MIMG" "../ports/sbase/bin/$p" "::/BIN/$p" 2>/dev/null
done
mcopy -o -i "$MIMG" "$KEY.pub" ::/root/.ssh/authorized_keys
printf 'hello from the machine\n' > "$WORK/greet.txt"
mcopy -o -i "$MIMG" "$WORK/greet.txt" ::/ST/greet.txt

rm -f "$SCRATCH/ssh.fifo"; mkfifo "$SCRATCH/ssh.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -nic user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/ssh.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/ssh.fifo"
sleep "$BOOT_WAIT"
send() { printf '%s\r' "$1" >&3; sleep "${2:-4}"; }

send 'ifconfig dhcp' 14
send 'dropbearkey -t ed25519 -f /etc/hostkey' 30
send 'dropbear -r /etc/hostkey -p 22 -E &' 6

# ssh takes -p for the port; scp takes -P, and lower case -p to scp
# means "preserve modification times" -- which makes the port number an
# extra SOURCE file, and the error it produces names the LOCAL path and
# reads exactly like a broken scp. It cost most of a night.
O="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
O="$O -o ConnectTimeout=15 -o LogLevel=ERROR -o BatchMode=yes"
SSHO="$O -p $PORT"
SCPO="$O -P $PORT"

ready=no
for i in $(seq 1 40); do
    r=$(timeout 15 ssh $SSHO root@127.0.0.1 'echo READY' 2>/dev/null)
    [ "$r" = READY ] && { ready=yes; break; }
    sleep 3
done

echo "=== checks ==="
[ "$ready" = yes ]
check "the machine accepts an ssh connection and runs a command" $?

if [ "$ready" != yes ]; then
    exec 3>&-; kill "$qemu_pid" 2>/dev/null
    echo; echo "  passed: $pass"; echo "  failed: $((fail + 1))"
    echo "RESULT: FAIL"; exit 1
fi

# --- ten of them ------------------------------------------------------
ok=0
for n in $(seq 1 10); do
    r=$(timeout 30 ssh $SSHO root@127.0.0.1 "echo OK$n" 2>/dev/null)
    [ "$r" = "OK$n" ] && ok=$((ok + 1))
done
test "$ok" -eq 10
check "TEN successive connections all work ($ok/10)" $?

# --- the machine can still use its own network afterwards -------------
send 'ping 10.0.2.2 3' 16
tr -d '\r' < "$LOG" | grep -q "3 sent, 3 received"
check "  and the machine can still ping out afterwards" $?

send 'ifconfig > /ST/if.out' 5
mcopy -n -o -i "$MIMG" ::/ST/if.out "$WORK/if.out" 2>/dev/null
awk '/^eth0/,0 {if (/TX [0-9]+ packets/) {for(i=1;i<=NF;i++) if ($i=="errors") print $(i-1)}}' \
    "$WORK/if.out" 2>/dev/null | head -1 | grep -qx "0"
check "  with no transmit errors at all" $?

# --- scp, both directions --------------------------------------------
rm -f "$WORK/fetched.txt"
timeout 45 scp -O $SCPO root@127.0.0.1:/ST/greet.txt "$WORK/fetched.txt" \
    > /dev/null 2>&1
cmp -s "$WORK/greet.txt" "$WORK/fetched.txt"
check "scp fetches a file OFF the machine, byte for byte" $?

printf 'pushed by scp\n' > "$WORK/push.txt"
timeout 45 scp -O $SCPO "$WORK/push.txt" root@127.0.0.1:/ST/push.txt \
    > /dev/null 2>&1
r=$(timeout 30 ssh $SSHO root@127.0.0.1 'cat /ST/push.txt' 2>/dev/null)
test "$r" = "pushed by scp"
check "  and puts one ON it" $?

# --- rsync over that ssh ---------------------------------------------
printf 'by rsync\n' > "$WORK/rs.txt"
timeout 90 rsync -e "ssh $SSHO" -rlt "$WORK/rs.txt" \
    root@127.0.0.1:/ST/rs.txt > /dev/null 2>&1
r=$(timeout 30 ssh $SSHO root@127.0.0.1 'cat /ST/rs.txt' 2>/dev/null)
test "$r" = "by rsync"
check "rsync over ssh transfers a file to the machine" $?

send 'echo ALL-DONE' 3
sleep 1
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/ssh.fifo"

tr -d '\r' < "$LOG" > "$WORK/session.txt"
! grep -qE "panic|exception at|DOUBLE MMU" "$WORK/session.txt"
check "no panic, no kernel exception" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
