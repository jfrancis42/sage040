# awk on SuckOS

The one true awk -- Kernighan's, the awk of *The AWK Programming
Language*, second edition -- built against picolibc and running as
`/bin/awk`.

```bash
make libc                  # once: picolibc
make -C ports/awk          # fetch, build ./awk
make -C ports/awk install  # onto hd.img as /BIN/AWK
make awktest               # its tests, on the machine
```

## The source is not here

Its licence is Lucent's permissive one, not the GPL, so `build.sh`
fetches it into `~/m68k/src` and builds it from there. The version is
Debian's copy of upstream's 2025-12-25 snapshot, pinned by checksum:
the upstream repository, github.com/onetrue-awk/awk, no longer answers,
and Debian's pool keeps every release it has shipped. No patches.

bison makes the parser and `maketab`, a host program, writes
`proctab.c` from its token numbers; the rest is cross-compiled, linked
against `/lib/libc.so`.

## What it needed from the system

awk itself needed nothing changed. The machine did:

- **Programs started from the shell began in the root**, whatever `cd`
  had said -- spawn never passed the working directory on. awk was the
  first program to open a relative name after a `cd`.
- **picolibc's stdout was line buffered even into a file or a pipe**,
  against POSIX, so output interleaved with stderr unlike anywhere else
  (and every line into a pipe was a system call).
- **`setlocale(LC_CTYPE, "")` ignored the environment**, so no program
  could be given a UTF-8 locale; and picolibc was built without
  multibyte locales at all (`mb-capable`).
- **`ungetc` held one character.** awk pushes back a partly-matched
  multibyte record separator byte by byte, as glibc and the BSDs allow.
- **`<signal.h>` had no `FPE_` codes**, which awk's SIGFPE handler names.

Each is a patch in `libc/patches/` or a kernel change, with a test.

## Tests

`kernel/awktest.sh` runs awk's own `bugs-fixed/` regression tests on
the machine and compares each output, on the host, with upstream's
expected output; and eleven tests of its own (`tests/`) -- fields,
numbers, strings, regular expressions, arrays, pipes, files, getline,
functions -- compared with what the same awk source prints when built
for the host. Two upstream tests are skipped until the tools they call
exist: `space` pipes into `sort`, and `system-status` uses `kill -KILL
$$`.
