#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# gittest.sh - git on the machine (ports/git).
#
# A repository git made and git reads back proves only that it agrees
# with itself. So repositories cross over with the HOST's git both ways:
#
#   made on the machine   copied off the disk afterwards; the host's
#                         `git fsck --full` must find every object
#                         sound, and its log and tree must be what the
#                         machine was told to commit
#   made on the host      put on the disk; the machine must read its
#                         history, check out an old commit and get the
#                         old bytes, and fsck it too
#
# In between, the things git does with processes and the network:
# a clone (upload-pack and fetch-pack talking down a pipe), a real
# three-way merge, gc into a packfile, a #!/bin/sh command
# (git-submodule is a shell script), and a clone over HTTP from a server
# on the host, which is git-remote-http -- curl and OpenSSL.
#
# The disk is laid out by the real install rules, so /bin/sh is bash.
# git's pager is cat: on a terminal it pages through less, which the
# real disk has (ports/less) and this one does not need.

set -u
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-git.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "gittest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/gittest.log"; FIFO="$SCRATCH/git.fifo"; T="$SCRATCH/git.tmp"
rm -f "$LOG" "$FIFO" "$DISK"; rm -rf "$T"; mkdir -p "$T"
HTTP_PORT=${HTTP_PORT:-18765}

inst() {
    make -s -C "$1" install DISK="$DISK" DISK_MB=160 >> "$T/install.log" 2>&1 || {
        echo "gittest: make -C $1 install failed" >&2; tail -20 "$T/install.log" >&2
        exit 1; }
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
for p in bash sbase sed grep git; do
    make -s -C ../ports/$p >/dev/null 2>&1 || { echo "gittest: ports/$p failed" >&2; exit 1; }
done

echo "=== installing with the real rules ==="
inst .
inst ../ldso
inst ../system
for p in bash sbase sed grep git; do inst ../ports/$p; done

# --- the host's repository, for the machine to read -----------------
H="$T/hostrepo"
git init -q -b main "$H"
git -C "$H" config user.name "Host Person"; git -C "$H" config user.email host@example.com
printf 'first version\n' > "$H/file.txt"
git -C "$H" add file.txt; git -C "$H" commit -q -m "host: first"
printf 'second version\n' > "$H/file.txt"; printf 'more\n' > "$H/other.txt"
git -C "$H" add -A; git -C "$H" commit -q -m "host: second"
HOST_HEAD=$(git -C "$H" rev-parse HEAD)
HOST_FIRST=$(git -C "$H" rev-parse HEAD~1)
fsimg put -r "$H" /hostrepo
# A bare copy, served over plain ("dumb") HTTP from the host.
git clone -q --bare "$H" "$T/www/served.git"
git -C "$T/www/served.git" update-server-info
( cd "$T/www" && exec python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 \
      > "$T/http.log" 2>&1 ) &
http_pid=$!

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
    -nic user,id=n0 \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"
wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() {                         # run TAG COMMAND [TENTHS/2]
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" "${3:-1500}" || echo "    (timed out: $1)"
}

sleep 5
G=/usr/bin/git
run VER  "$G --version"
run ID   "$G config --global user.name 'Machine Person'; $G config --global user.email m@example.com; $G config --global init.defaultBranch main; $G config --global core.pager cat"
# --- made on the machine -----------------------------------------
run INIT "$G init -q /r"
run CD   'cd /r'
run C1   "echo one > a.txt; echo two > b.txt; $G add a.txt b.txt; $G commit -q -m 'machine: first'"
run C2   "echo changed >> a.txt; $G commit -q -a -m 'machine: second'"
run DIFF "echo third >> b.txt; $G diff"
run C3   "$G commit -q -a -m 'machine: third'"
# A real merge: both sides change different files.
run BR   "$G checkout -q -b side HEAD~1; echo side > c.txt; $G add c.txt; $G commit -q -m 'machine: side'"
run MERGE "$G checkout -q main; $G merge -q --no-edit side"
run LOG  "$G log --oneline | /bin/wc -l"
run GC   "$G gc -q"
run SUB  "$G submodule status"
run CDB  'cd /'
# --- a clone through upload-pack and fetch-pack --------------------
run CLONE "$G clone -q /r /c"
run CLOG  "$G -C /c log -1 --format=%s"
# --- made on the host ----------------------------------------------
run HLOG  "$G -C /hostrepo log --format=%H:%s"
run HCO   "$G -C /hostrepo checkout -q $HOST_FIRST && /bin/cat /hostrepo/file.txt"
run HFSCK "$G -C /hostrepo fsck --full"
# --- over HTTP, from the host ----------------------------------------
run NET   'ifconfig dhcp' 400
run HTTP  "$G clone -q http://10.0.2.2:$HTTP_PORT/served.git /h" 3000
run HTTPL "$G -C /h log -1 --format=%H"
printf 'echo ALL-DONE\r' >&3
wait_for 'ALL-DONE' 300
printf 'sync; halt\r' >&3
sleep 4
exec 3>&-; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
kill "$http_pid" 2>/dev/null; wait "$http_pid" 2>/dev/null
rm -f "$FIFO"
CLEAN="$SCRATCH/gittest-clean.tmp"
tr -d '\r' < "$LOG" | sed -e 's/\x1b\][^\x1b]*\x1b\\//g' -e 's/\x1b\[[?0-9;]*[a-zA-Z]//g' > "$CLEAN"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
st() { sed -n "s/^$1-END //p" "$CLEAN" | head -1; }
# Everything between the end of one command and the end of the next.
out() { awk -v a="$1" -v b="$2" '$0 ~ "^"b"-END" {on=0} on {print} $0 ~ "^"a"-END" {on=1}' "$CLEAN"; }

echo "=== checks: on the machine ==="
grep -aqx 'git version 2.56.0' "$CLEAN" && [ "$(st VER)" = 0 ]
check "git --version" $?
for t in INIT C1 C2 C3 BR MERGE GC; do [ "$(st $t)" = 0 ] || break; done
[ "$(st INIT)$(st C1)$(st C2)$(st C3)$(st BR)$(st MERGE)$(st GC)" = 0000000 ]
check "init, commits, a branch, a merge and gc all exit 0" $?
out C2 DIFF | grep -aqx '+third' && out C2 DIFF | grep -aqx 'diff --git a/b.txt b/b.txt'
check "git diff shows the uncommitted line" $?
out MERGE LOG | grep -aqx ' *5'
check "the log has five commits: three, the side branch's, and the merge" $?
[ "$(st SUB)" = 0 ]
check "git submodule, a #!/bin/sh script, runs" $?
[ "$(st CLONE)" = 0 ] && out CLONE CLOG | grep -aqx "Merge branch 'side'"
check "a clone, through upload-pack and fetch-pack, has the merge at its tip" $?
out CDB HLOG | grep -aqx "$HOST_HEAD:host: second"
check "the HOST's repository: the machine reads its history" $?
out HLOG HCO | grep -aqx 'first version'
check "  and checks out the first commit's bytes" $?
[ "$(st HFSCK)" = 0 ]
check "  and fsck --full on it is clean" $?
[ "$(st HTTP)" = 0 ] && out HTTP HTTPL | grep -aqx "$HOST_HEAD"
check "a clone over HTTP from the host, through git-remote-http" $?

echo "=== checks: the machine's repository, read by the HOST's git ==="
rm -rf "$T/r"
# get -r SRC DEST makes DEST/<basename of SRC>.
PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" get -r /r "$T" >/dev/null 2>&1
git -C "$T/r" fsck --full --strict >/dev/null 2>"$T/fsck.err"
check "git fsck --full --strict finds every object sound" $?
[ "$(git -C "$T/r" log --format=%s | tr '\n' '|')" = "Merge branch 'side'|machine: side|machine: third|machine: second|machine: first|" ]
check "the history is what the machine was told to commit" $?
[ "$(git -C "$T/r" log -1 --format='%an <%ae>' HEAD~1)" = "Machine Person <m@example.com>" ]
check "  by the user the machine was configured with" $?
[ "$(git -C "$T/r" show HEAD:a.txt)" = "$(printf 'one\nchanged')" ] \
    && [ "$(git -C "$T/r" show HEAD:c.txt)" = side ] \
    && [ "$(git -C "$T/r" show HEAD:b.txt)" = "$(printf 'two\nthird')" ]
check "the merged tree holds both sides' files" $?
git -C "$T/r" count-objects -v > "$T/count" 2>/dev/null
grep -qx 'count: 0' "$T/count" && grep -qx 'packs: 1' "$T/count" && grep -qx 'in-pack: 15' "$T/count"
check "gc packed the loose objects into a packfile the host can read" $?

grep -aqE 'panic|bus error|address error|DOUBLE MMU FAULT' "$CLEAN"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
