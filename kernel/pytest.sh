#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# pytest.sh - CPython on this machine.
#
# The interpreter is a 6 MB statically linked program and the standard
# library is 45 MB of files, so this suite makes a disk of its own
# rather than the 16 MB the others use.
#
# What it checks, in the order the machine has to manage it:
#
#   1. that the interpreter starts at all, and says which Python it is;
#   2. that the things a machine has to provide are right -- sys.platform,
#      byte order, the size of a pointer, the maximum integer;
#   3. that the standard library IMPORTS, which is the part that reads
#      two thousand files off a FAT filesystem;
#   4. arithmetic, including the big integers and the floating point the
#      68040's FPU does;
#   5. the operating system underneath: files, directories, processes,
#      time, threads, sockets;
#   6. and a script run from a file, because that is how anything real
#      will use it.
#
# Each answer is compared with what the HOST's Python 3.14 says to the
# same question, which is the independent source: a check written by
# hand would only say what the author expected.

set -u

cd "$(dirname "$0")"

. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
PYSTAGE=$SRCDIR/build-python-sage040/stage
HOSTPY=${BUILD_PYTHON:-$(command -v python3)}

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-python.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/pytest.log"
rm -f "$LOG"
BOOT_WAIT=${BOOT_WAIT:-4}
DISK_MB=${PY_DISK_MB:-256}

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
if [ ! -x "$PYSTAGE/bin/python3" ]; then
    make -C ../ports/python stage || exit 1
fi

# The script the guest runs. It prints one line per question, and the
# host asks its own Python the same ones below.
cat > "$SCRATCH/pyprobe.py" <<'PYEOF'
import sys, os, platform, struct, math, time, json

def say(k, v):
    print("PY %s=%s" % (k, v))

say("version", "%d.%d.%d" % sys.version_info[:3])
say("platform", sys.platform)
say("byteorder", sys.byteorder)
say("pointer", struct.calcsize("P"))
say("maxsize", sys.maxsize)
say("machine", os.uname().machine)
say("sysname", os.uname().sysname)

# Arithmetic: big integers, and the FPU.
say("bigint", 2**256 // 3)
say("factorial", math.factorial(30))
say("float", repr(0.1 + 0.2))
say("sqrt2", repr(math.sqrt(2.0)))
say("expsum", repr(sum(math.exp(i / 8.0) for i in range(16))))

# Strings, sorting, dicts, comprehensions: the interpreter proper.
words = "the quick brown fox jumps over the lazy dog".split()
say("sorted", ",".join(sorted(words)))
say("counts", json.dumps({w: words.count(w) for w in sorted(set(words))},
                         sort_keys=True))
say("upper", "".join(w[0].upper() for w in words))
say("slice", "abcdefghij"[2:8:2])
say("fstring", f"{22/7:.6f}")

# The standard library, imported off the disk.
mods = ["os", "sys", "re", "json", "math", "random", "datetime", "hashlib",
        "collections", "itertools", "functools", "textwrap", "base64",
        "struct", "socket", "select", "threading", "subprocess", "tempfile",
        "shutil", "glob", "pickle", "copy", "argparse", "logging", "zlib",
        "binascii", "csv", "configparser", "decimal", "fractions",
        "statistics", "unicodedata", "curses", "termios", "sqlite3"]
ok, bad = [], []
for m in mods:
    # The name BEFORE the import, and flushed: a module that does not
    # merely fail but takes the interpreter down with it is otherwise
    # invisible -- there is no traceback for a segmentation fault.
    sys.stdout.write("PY importing %s\n" % m)
    sys.stdout.flush()
    try:
        __import__(m)
        ok.append(m)
    except Exception as e:
        bad.append("%s(%s)" % (m, type(e).__name__))
say("imported", len(ok))
say("failed_imports", ",".join(bad) if bad else "none")

# Regular expressions and hashing, against values the host computes too.
import re, hashlib, base64, zlib
say("re", re.sub(r"(\w+) (\w+)", r"\2 \1", "hello world once again"))
say("sha256", hashlib.sha256(b"SuckOS on a 68040").hexdigest())
say("md5", hashlib.md5(b"SuckOS").hexdigest())
say("b64", base64.b64encode(b"Sage040").decode())
say("crc32", zlib.crc32(b"the quick brown fox"))
say("zlib", len(zlib.compress(b"x" * 1000)))
say("zlibok", zlib.decompress(zlib.compress(b"y" * 1000)) == b"y" * 1000)

# The operating system underneath. The directory comes from tempfile,
# because this script runs on the HOST as well -- where /PYTMP is not
# somewhere anybody may write, and every answer below would be empty.
import tempfile
d = tempfile.mkdtemp()
with open(d + "/a.txt", "w") as f:
    f.write("written by python\n" * 10)
with open(d + "/a.txt") as f:
    data = f.read()
say("filelen", len(data))
say("stat", os.stat(d + "/a.txt").st_size)
say("listdir", ",".join(sorted(os.listdir(d))))
os.rename(d + "/a.txt", d + "/b.txt")
say("renamed", os.path.exists(d + "/b.txt") and not os.path.exists(d + "/a.txt"))
os.remove(d + "/b.txt")
say("removed", not os.path.exists(d + "/b.txt"))

os.rmdir(d)
say("cwd_is_a_path", os.getcwd().startswith("/"))
say("pid_is_tgid", os.getpid() > 0)
say("umask", oct(os.umask(0o22)))
say("nofile", __import__("resource").getrlimit(__import__("resource").RLIMIT_NOFILE)[0])

# Threads, which is what most of the work under this port was for.
import threading
total = 0
lock = threading.Lock()
def worker(n):
    global total
    for _ in range(n):
        with lock:
            total += 1
ts = [threading.Thread(target=worker, args=(250,)) for _ in range(4)]
for t in ts: t.start()
for t in ts: t.join()
say("threads", total)
say("thread_names", len(threading.enumerate()))

# Time, and the time zone.
os.environ["TZ"] = "MST7MDT,M3.2.0,M11.1.0"
time.tzset()
t = 1600000000
say("gmtime", time.strftime("%Y-%m-%d %H:%M", time.gmtime(t)))
say("localtime", time.strftime("%Y-%m-%d %H:%M %Z", time.localtime(t)))
say("mktime_roundtrip", int(time.mktime(time.localtime(t))) == t)
del os.environ["TZ"]
time.tzset()

# A subprocess: fork and exec, from Python.
import subprocess
try:
    r = subprocess.run(["/BIN/echo", "hello-from-subprocess"],
                       capture_output=True, text=True, timeout=30)
    say("subprocess", r.stdout.strip())
except Exception as e:
    say("subprocess", "FAILED:%s" % type(e).__name__)

print("PY DONE")
PYEOF

echo "=== preparing $DISK (${DISK_MB} MB) ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count="$DISK_MB" status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" -s 64 "$DISK" \
    $(( (DISK_MB * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mmd -i "$MIMG" ::/BIN ::/lib
mcopy -o -i "$MIMG" ../ldso/ld.so ::/lib/ld.so
mcopy -o -i "$MIMG" "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" ::/lib/libc.so
for p in ../system/sh ../ports/sbase/bin/echo; do
    [ -x "$p" ] && mcopy -o -i "$MIMG" "$p" "::/BIN/$(basename "$p")"
done

echo "    the interpreter and the standard library ($(du -sh "$PYSTAGE" | cut -f1))"
mmd -i "$MIMG" ::/usr ::/usr/local ::/usr/local/bin ::/usr/local/lib
mcopy -o -i "$MIMG" "$PYSTAGE/bin/python3" ::/usr/local/bin/python3
mcopy -o -s -i "$MIMG" "$PYSTAGE/lib/python3.14" ::/usr/local/lib/
mcopy -o -i "$MIMG" "$SCRATCH/pyprobe.py" ::/PROBE.PY

rm -f "$SCRATCH/py.fifo"
mkfifo "$SCRATCH/py.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/py.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/py.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 "${2:-3000}"); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

echo "=== running (this is a 25 MHz 68040 reading 2,000 files) ==="
printf '/usr/local/bin/python3 -V\r' >&3
wait_for "Python 3." 1200
sleep 1
printf '/usr/local/bin/python3 /PROBE.PY\r' >&3
wait_for "PY DONE" 9000
sleep 1
printf 'echo STILL-HERE\r' >&3
wait_for "STILL-HERE" 600

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/py.fifo"
tr -d '\r' < "$LOG" > "$SCRATCH/py-clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/py-clean.tmp" | tail -80

# The same script on the host, for the answers that must agree.
"$HOSTPY" "$SCRATCH/pyprobe.py" > "$SCRATCH/py-host.tmp" 2>&1 || true

echo "=== checks ==="

guest() { sed -n "s/^PY $1=//p" "$SCRATCH/py-clean.tmp" | head -1; }
host()  { sed -n "s/^PY $1=//p" "$SCRATCH/py-host.tmp"  | head -1; }

same() {                        # same KEY DESCRIPTION
    local g h
    g=$(guest "$1"); h=$(host "$1")
    if [ -n "$g" ] && [ "$g" = "$h" ]; then
        check "$2 ($g)" 0
    else
        check "$2 -- guest '$g', host '$h'" 1
    fi
}

# The 68040 computes exp() in 80-bit extended precision and rounds once
# at the end; an x86-64 host computes it in 64-bit doubles. Both are
# correct and they differ in the last place, which is a fact about the
# hardware rather than a fault -- so this is the one comparison with a
# tolerance, and it says so.
close() {                       # close KEY TOLERANCE DESCRIPTION
    local g h
    g=$(guest "$1"); h=$(host "$1")
    if [ -z "$g" ] || [ -z "$h" ]; then
        check "$3 -- guest '$g', host '$h'" 1
        return
    fi
    if awk -v a="$g" -v b="$h" -v t="$2" \
           'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; then
        check "$3 (guest $g, host $h)" 0
    else
        check "$3 -- guest '$g', host '$h'" 1
    fi
}

is() {                          # is KEY WANTED DESCRIPTION
    local g
    g=$(guest "$1")
    if [ "$g" = "$2" ]; then check "$3 ($g)" 0; else check "$3 -- got '$g', wanted '$2'" 1; fi
}

grep -q "^Python 3\.14" "$SCRATCH/py-clean.tmp"
check "the interpreter starts and says which it is" $?

grep -qx "PY DONE" "$SCRATCH/py-clean.tmp"
check "the probe script ran to the end" $?

echo "--- the machine"
same version   "the same version as the host's"
is   platform  linux  "sys.platform"
is   byteorder big    "byte order is big-endian, unlike the host"
is   pointer   4      "a pointer is four bytes"
is   machine   m68040 "os.uname().machine -- the CPU, as uname reports it"
is   sysname   SuckOS "os.uname().sysname"
is   maxsize   2147483647 "sys.maxsize: this is a 32-bit machine"

echo "--- arithmetic"
same bigint    "a 256-bit integer divided by three"
same factorial "30 factorial"
same float     "0.1 + 0.2, to the last bit"
same sqrt2     "the square root of two, on the FPU"
close expsum 1e-12 "a sum of sixteen exponentials, to within the last bit"

echo "--- the interpreter"
same sorted    "sorting"
same counts    "a dict comprehension, as JSON"
same upper     "a generator expression"
same slice     "an extended slice"
same fstring   "an f-string with a format spec"

echo "--- the standard library"
same imported  "modules imported off the disk"
# sqlite3 is not built here (no library) and the host has it, so the
# two lists differ by exactly that, and by nothing else.
g=$(guest failed_imports)
case "$g" in
    none|"sqlite3(ModuleNotFoundError)")
        check "the only module missing is sqlite3, which is not built ($g)" 0 ;;
    *)  check "modules that failed to import: $g" 1 ;;
esac
same re        "regular expression substitution"
same sha256    "SHA-256"
same md5       "MD5"
same b64       "base64"
same crc32     "CRC-32"
same zlib      "zlib compressed to the same size"
is   zlibok    True "  and decompressed back"

echo "--- the operating system"
same filelen   "a file written and read back"
same stat      "stat says the same size"
same listdir   "listdir"
is   renamed   True "rename"
is   removed   True "remove"
same threads   "four threads and a lock, counting to 1000"
is   pid_is_tgid True "getpid"
same gmtime    "gmtime"
same localtime "localtime in a zone with daylight saving"
is   mktime_roundtrip True "mktime is localtime backwards"
is   nofile    64 "the descriptor limit is this machine's"
is   cwd_is_a_path True "getcwd returns an absolute path"
is   subprocess hello-from-subprocess "a subprocess"

! grep -qE "panic|DOUBLE MMU|exception at" "$SCRATCH/py-clean.tmp"
check "no panic, no kernel exception" $?

grep -qx "STILL-HERE" "$SCRATCH/py-clean.tmp"
check "the machine is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
