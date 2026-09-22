# GNU sed on SuckOS

GNU sed 4.10, built against picolibc and running as `/bin/sed`.

```bash
make libc                  # once: picolibc
make -C ports/sed          # fetch, verify, cross-configure, build ./sed
make -C ports/sed install  # onto hd.img as /BIN/SED
make sedtest               # its tests, on the machine
```

## The source is not here

`build.sh` fetches the release from ftp.gnu.org, checks it against a
pinned SHA-256 (the release's GPG signature was checked against the GNU
keyring when it was pinned), and builds it out of tree in `~/m68k/src`.
`ports/cross.sh` holds what every GNU-style port needs to cross-configure
for this machine -- see its comments for why the host is
`m68k-unknown-elf` rather than `m68k-linux-gnu`. No patches to sed.

## What it needed from the system

sed itself needed nothing changed. picolibc did (each a patch in
`libc/patches/` or a file in the m68k backend, with a test):

- **`<stdio_ext.h>`** -- `__fwriting`, `__freading`, `__fpending`,
  `__fpurge`, `__freadahead`: gnulib asks a FILE about its state through
  these, and without them fell back to reaching inside FILE, which it
  does not know how to do for picolibc.
- **`getchar_unlocked` and `putchar_unlocked`** were macros taking one
  argument too many.
- **`sysconf`** was not in libc.so, and the static one answered every
  question with POSIX's minimum.
- **`getprogname` and `program_invocation_name`**: nothing recorded
  argv[0]. crt0 now does, before main.

## Tests

`kernel/sedtest.sh` runs the scripts in `tests/` on the machine --
substitution flags, EREs, every address form, the hold space, branches,
a/i/c/=/l/y/q, r/R/w, -n/-s/-z/-i, case conversion over UTF-8, and 3000
lines of volume -- and compares each output, and the files written, with
what the same sed source prints when built for the host. It also checks
that what was compared is real: two runs that failed the same way would
match. sed's own test suite is shell scripts over coreutils; it waits
for bash and the utilities (tasks 28 and 29).
