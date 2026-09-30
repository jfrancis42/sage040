#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# perltest.sh - Perl 5 on the machine (ports/perl).
#
# A Perl that starts has not been shown to work: its XS extensions are
# shared objects that DynaLoader dlopen()s, its integers are 64-bit on
# a 32-bit CPU, it is big-endian, and its Config is what anything built
# on the machine later is built from. So the checks go through each of
# those, and wherever there is an answer that does not come from this
# Perl -- a digest, a compressed stream, a byte order -- the HOST
# computes it and compares.
#
#   1. facts: version, archname, 64-bit integers, pack's byte order,
#      the XS modules (POSIX, List::Util, Digest::MD5/SHA, Storable,
#      Data::Dumper, Time::HiRes, Socket, Encode, Compress::Zlib),
#      Errno, fork and pipes, qx//, regular expressions, Unicode
#   2. a #!/usr/bin/perl script run by name, from both shells
#   3. perldoc, a large pure-Perl program
#   4. an XS module BUILT ON THE MACHINE, twice: with ExtUtils::ParseXS
#      and ExtUtils::CBuilder, which is Config and the native gcc alone;
#      and with MakeMaker and make, which is how CPAN does it
#
# The toolchain goes on the scratch disk for 4, as in nativetest.sh, so
# this disk is the big one.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
PERLOUT=$SRCDIR/build-perl-sage040/sage040
BINOUT=$SRCDIR/build-binutils-native/sage040
GCCOUT=$SRCDIR/build-gcc-native/sage040
PV=5.44.0

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-perl.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "perltest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/perltest.log"; FIFO="$SCRATCH/perl.fifo"
WORK="$SCRATCH/perl.tmp"
rm -f "$LOG" "$FIFO"; rm -rf "$WORK"; mkdir -p "$WORK"
RAM=${PERL_RAM_MB:-256}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../ldso || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1
../ports/perl/build.sh >/dev/null || exit 1
../ports/make/build.sh >/dev/null || exit 1
make -s -C ../ports/gcc specs >/dev/null || exit 1
for d in "$BINOUT" "$GCCOUT"; do
    [ -d "$d" ] || { echo "perltest: no $d -- build ports/binutils and ports/gcc" >&2; exit 1; }
done

# --- the guest's scripts ------------------------------------------------
TEXT='The quick brown fox jumps over the lazy dog'
cat > "$WORK/facts.pl" <<EOF
use strict; use warnings;
use Config; use POSIX (); use List::Util qw(sum max first);
use Digest::MD5 qw(md5_hex); use Digest::SHA qw(sha256_hex);
use Storable qw(freeze thaw); use Data::Dumper; use Time::HiRes ();
use Socket qw(inet_aton inet_ntoa); use Encode qw(encode);
use Compress::Zlib (); use Errno qw(ENOENT); use JSON::PP ();
my \$t = '$TEXT';
print "VERSION=\$^V\n";
print "ARCH=\$Config{archname}\n";
print "IVSIZE=\$Config{ivsize}\n";
print "BIG=", 9007199254740993, "\n";
print "SHIFT=", 1 << 40, "\n";
print "PACKN=", unpack("H*", pack("N", 0x01020304)), "\n";
print "PACKL=", unpack("H*", pack("L", 0x01020304)), "\n";
print "PACKQ=", unpack("H*", pack("Q", 0x0102030405060708)), "\n";
print "PI=", sprintf("%.10f", 4 * atan2(1, 1)), "\n";
print "FLOOR=", POSIX::floor(-3.5), "\n";
print "DATE=", POSIX::strftime("%Y-%m-%d", gmtime(86400 * 365)), "\n";
print "SUM=", sum(1 .. 100), " MAX=", max(3, 9, 2), "\n";
print "MD5=", md5_hex(\$t), "\n";
print "SHA=", sha256_hex(\$t), "\n";
my \$s = thaw(freeze({ a => [1, 2, { b => 'c' }] }));
print "STORABLE=", \$s->{a}[2]{b}, \$s->{a}[1], "\n";
\$Data::Dumper::Indent = 0; \$Data::Dumper::Sortkeys = 1; \$Data::Dumper::Terse = 1;
print "DUMPER=", Dumper({ x => 1, y => [2, 3] }), "\n";
print "JSON=", JSON::PP->new->canonical->encode({ b => [1, "two"], a => undef }), "\n";
my \$t0 = Time::HiRes::time(); Time::HiRes::sleep(0.3);
my \$dt = Time::HiRes::time() - \$t0;
print "HIRES=", (\$dt >= 0.25 && \$dt < 2 ? "ok" : "bad \$dt"), "\n";
print "SOCKET=", inet_ntoa(inet_aton("127.0.0.1")), "\n";
print "UTF8=", length("\x{263A}"), ",", length(encode("UTF-8", "\x{263A}")), "\n";
open(my \$z, '>:raw', '/T/z.bin') or die;
print \$z Compress::Zlib::compress(\$t x 20); close \$z;
open(my \$nx, '<', '/no/such/file') and die;
print "ERRNO=", (\$!{ENOENT} ? "ENOENT" : "other"), ",", \$! + 0, "\n";
pipe(my \$r, my \$w) or die;
my \$pid = fork() // die;
if (!\$pid) { close \$r; print \$w "from child \$\$\n"; close \$w; exit 7; }
close \$w; my \$line = <\$r>; waitpid(\$pid, 0);
print "FORK=", (\$line =~ /^from child \d+\$/ ? "line" : "noline"), ",", \$? >> 8, "\n";
my \$q = qx(/bin/echo spawned); chomp \$q; print "QX=\$q\n";
"2026-09-30T14:05" =~ /^(\d+)-(\d+)-(\d+)T(\d+):(\d+)\$/ or die;
print "RE=\$1/\$2/\$3 \$4h\n";
print "SORT=", join(",", sort { \$a <=> \$b } (10, 9, 100, 1)), "\n";
print "FACTS-DONE\n";
EOF
cat > "$WORK/args.pl" <<'EOF'
#!/usr/bin/perl -w
print "ARGS=$0|", join("|", @ARGV), "|", ($^W ? "w" : "nw"), "\n";
EOF
cat > "$WORK/Sage.xs" <<'EOF'
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

MODULE = Sage   PACKAGE = Sage

IV
add(a, b)
    IV a
    IV b
  CODE:
    RETVAL = a + b;
  OUTPUT:
    RETVAL

const char *
hello()
  CODE:
    RETVAL = "hello from C";
  OUTPUT:
    RETVAL
EOF
cat > "$WORK/Sage.pm" <<'EOF'
package Sage;
our $VERSION = '0.01';
require XSLoader;
XSLoader::load('Sage', $VERSION);
1;
EOF
# Compiled and linked by Config alone: this is exactly what MakeMaker
# would do, without make in the way.
cat > "$WORK/cbuild.pl" <<'EOF'
use strict; use warnings;
use ExtUtils::ParseXS; use ExtUtils::CBuilder; use Config;
chdir '/X1' or die;
ExtUtils::ParseXS->new->process_file(filename => 'Sage.xs', output => 'Sage.c');
my $b = ExtUtils::CBuilder->new(quiet => 0);
my $o = $b->compile(source => 'Sage.c');
my $so = $b->link(objects => [$o], module_name => 'Sage',
                  lib_file => 'auto/Sage/Sage.so');
print "CBUILT=$so\n";
EOF
cat > "$WORK/Makefile.PL" <<'EOF'
use ExtUtils::MakeMaker;
WriteMakefile(NAME => 'Sage', VERSION_FROM => 'Sage.pm');
EOF

echo "=== what the host says ==="
EXP_MD5=$(printf '%s' "$TEXT" | md5sum | cut -d' ' -f1)
EXP_SHA=$(printf '%s' "$TEXT" | sha256sum | cut -d' ' -f1)
echo "  md5 $EXP_MD5"

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=512 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /etc /tmp /T /X1 /X2 /usr /usr/bin /usr/lib /usr/include /usr/lib/perl5; do
    fsimg mkdir $d
done
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
# The layout `make install` makes: bash is /bin/sh, which MakeMaker's
# Makefiles run their recipes with, and the system shell is /bin/msh.
put_shells bash
fsimg put -m 644 ../system/passwd /etc/passwd
for p in echo cat ls rm mkdir cp true chmod touch mv test; do
    fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p
done
# GNU make, as `make install` puts it: MakeMaker's Makefiles are beyond
# sbase's POSIX make.
fsimg put -m 755 "$SRCDIR/build-make-sage040/sage040/bin/make" /bin/make
echo "    perl"
for f in "$PERLOUT"/bin/*; do fsimg put -m 755 "$f" "/usr/bin/$(basename "$f")"; done
fsimg put -r "$PERLOUT/lib/perl5/$PV" "/usr/lib/perl5/$PV"
echo "    the toolchain"
for b in "$BINOUT"/bin/*; do fsimg put -m 755 "$b" "/usr/bin/$(basename "$b")"; done
( cd "$GCCOUT" && find . -type d ) | sed 's|^\./||' | grep -v '^\.$' | sort |
    while read -r d; do fsimg mkdir "/usr/$d"; done
fsimg put -r "$GCCOUT" /usr
gcc_vdir=$(cd "$GCCOUT" && find lib/gcc -mindepth 2 -maxdepth 2 -type d | head -1)
fsimg put "$SRCDIR/build-gcc-native/specs.sage040" "/usr/$gcc_vdir/specs"
fsimg put -r "$SAGE_LIBC/include" /usr/include
for f in libc.a libc.so liblinux.a libm.a libdl.a librt.a libpthread.a libutil.a libcrypt.a; do
    fsimg put "$SAGE_LIBC/lib/$f" /usr/lib/
done
. ../ports/cross.sh; trap - EXIT
fsimg put "$("$CROSS_CC" -mcpu=68040 -print-libgcc-file-name)" /usr/lib/
fsimg put ../libc/crt0.o ../libc/crt0-dyn.o ../libc/sage040.ld /usr/lib/
fsimg put "$WORK/facts.pl" /T/facts.pl
fsimg put -m 755 "$WORK/args.pl" /T/args.pl
fsimg put "$WORK/cbuild.pl" /T/cbuild.pl
fsimg put "$WORK/Sage.xs" "$WORK/Sage.pm" /X1/
fsimg mkdir /X1/auto; fsimg mkdir /X1/auto/Sage
fsimg put "$WORK/Sage.xs" "$WORK/Sage.pm" "$WORK/Makefile.PL" /X2/

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM" -kernel ../bootrom/bootrom.elf \
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
run() {                         # run TAG COMMAND [TENTHS]
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" "${3:-3000}" || echo "    (timed out: $1)"
}

sleep 5
echo "=== running (perl on a 68040 is not quick) ==="
run FACTS '/usr/bin/perl /T/facts.pl' 6000
run K-ARGS '/T/args.pl a "b c"'
run DOC '/usr/bin/perldoc -T -f substr > /T/doc.txt' 18000
run DOCN '/bin/cat /T/doc.txt | /usr/bin/perl -ne "print if /^ +substr EXPR/"'
run CB '/usr/bin/perl /T/cbuild.pl' 18000
# Each step its own command, so that the status echoed is ITS status:
# `cd /X2; make; cd /` reported cd's, and a make that stopped on the
# first line of the Makefile passed.
run CBRUN '/usr/bin/perl -I/X1 -MSage -le "print q(XS1=), Sage::add(40, 2), q( ), Sage::hello()"'
run GMAKE '/bin/make --version'
run X2 'cd /X2'
run MMPL '/usr/bin/perl Makefile.PL' 6000
run MAKE '/bin/make' 18000
run MMRUN '/usr/bin/perl -Mblib -MSage -le "print q(XS2=), Sage::add(-5, 3)"'
run ROOT 'cd /'
printf '/bin/bash\r' >&3
sleep 3
run B-ARGS '/T/args.pl x'
printf 'exit\r' >&3
sleep 2
printf 'echo ALL-DONE\r' >&3
wait_for 'ALL-DONE' 100
printf 'halt\r' >&3
sleep 3
exec 3>&-; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
rm -f "$FIFO"

CLEAN="$SCRATCH/perltest-clean.tmp"
# -a everywhere below would do; stripping the terminal's escapes and
# gcc's hyperlinks makes the log text, so grep reads all of it.
tr -d '\r' < "$LOG" | sed -e 's/\x1b\][^\x1b]*\x1b\\//g' -e 's/\x1b\[[?0-9;]*[a-zA-Z]//g' > "$CLEAN"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
val() { sed -n "s/^$1=//p" "$CLEAN" | head -1; }
is() {                          # is KEY EXPECTED LABEL
    local got; got=$(val "$1")
    [ "$got" = "$2" ]
    check "$3 ($1=$got)" $?
}

echo "=== checks: the interpreter and its modules ==="
grep -qx 'FACTS-DONE' "$CLEAN" && grep -qx 'FACTS-END 0' "$CLEAN"
check "the facts script ran to the end and exited 0" $?
is VERSION "v$PV" "perl says it is $PV"
is ARCH m68k-linux "archname"
is IVSIZE 8 "integers are 64-bit"
is BIG 9007199254740993 "2**53+1 is exact, which a double cannot hold"
is SHIFT 1099511627776 "1 << 40"
is PACKN 01020304 "pack N is big-endian, as always"
is PACKL "$(perl -e 'print unpack("H*", pack("L>", 0x01020304))')" "pack L is NATIVE, and native is big-endian"
is PACKQ 0102030405060708 "pack Q, a 64-bit quad, native order"
is PI 3.1415926536 "floating point: 4*atan2(1,1)"
is FLOOR -4 "POSIX::floor, an XS function"
is DATE 1971-01-01 "POSIX::strftime"
is SUM "5050 MAX=9" "List::Util, XS"
is MD5 "$EXP_MD5" "Digest::MD5 agrees with the host's md5sum"
is SHA "$EXP_SHA" "Digest::SHA agrees with the host's sha256sum"
is STORABLE c2 "Storable freeze and thaw"
is DUMPER "{'x' => 1,'y' => [2,3]}" "Data::Dumper"
is JSON '{"a":null,"b":[1,"two"]}' "JSON::PP"
is HIRES ok "Time::HiRes: a 0.3 s sleep measures as one"
is SOCKET 127.0.0.1 "Socket"
is UTF8 1,3 "Unicode: one character, three bytes of UTF-8"
is ERRNO ENOENT,2 "Errno: \$!{ENOENT}, and ENOENT is 2"
is FORK line,7 "fork, a pipe, and the child's exit status"
is QX spawned "qx// runs a program"
is RE "2026/09/30 14h" "regular expression captures"
is SORT 1,9,10,100 "a numeric sort"

echo "=== checks: scripts and programs ==="
grep -qx 'ARGS=/T/args.pl|a|b c|w' "$CLEAN"
check "a #!/usr/bin/perl -w script runs by name from the kernel shell, -w and all" $?
grep -qx 'ARGS=/T/args.pl|x|w' "$CLEAN"
check "  and from bash" $?
grep -qx 'DOCN-END 0' "$CLEAN" && grep -qE '^ +substr EXPR,OFFSET' "$CLEAN"
check "perldoc -f substr finds and formats perlfunc's entry" $?

echo "=== checks: an XS module built on the machine ==="
grep -qx 'CBUILT=auto/Sage/Sage.so' "$CLEAN"
check "ParseXS and CBuilder compile and link it with the native gcc, from Config" $?
grep -qx 'XS1=42 hello from C' "$CLEAN"
check "  and perl loads it and calls into it" $?
grep -qE '^GNU Make 4\.4\.1$' "$CLEAN" && grep -qx 'GMAKE-END 0' "$CLEAN"
check "/bin/make is GNU make" $?
grep -qx 'MMPL-END 0' "$CLEAN" && grep -qx 'Writing Makefile for Sage' "$CLEAN"
check "MakeMaker writes a Makefile" $?
grep -qx 'MAKE-END 0' "$CLEAN" && ! grep -q '^make: \*\*\*' "$CLEAN"
check "  make builds it" $?
grep -qx 'XS2=-2' "$CLEAN"
check "  and -Mblib loads what make built" $?

echo "=== checks: from the host ==="
fsimg get /T/z.bin "$WORK/z.bin" >/dev/null 2>&1
got=$(python3 -c "import zlib,sys; print(zlib.decompress(open(sys.argv[1],'rb').read()).decode())" \
      "$WORK/z.bin" 2>/dev/null)
[ "$got" = "$(for i in $(seq 20); do printf '%s' "$TEXT"; done)" ]
check "Compress::Zlib's output decompresses on the host to what went in" $?

grep -qE 'panic|bus error|address error|DOUBLE' "$CLEAN"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
