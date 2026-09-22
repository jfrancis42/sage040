# GNU bash on SuckOS

GNU bash 5.3.20 — the 5.3 release with all twenty of GNU's published
patches — built against picolibc and running as `/bin/bash`.

```bash
make libc                   # once: picolibc
make -C ports/bash          # fetch, verify, patch, cross-configure, build ./bash
make -C ports/bash install  # onto hd.img as /BIN/BASH
make bashtest               # the language and part of bash's own suite, on the machine
make bashsuite              # every one of bash's 83 tests: hours, not minutes
```

It is installed **beside** `/bin/sh`, not in place of it. The system's own
shell is what `/etc/rc` runs, what the kernel starts, and what `system()`
and `sh -c` reach; bash is a program somebody chooses to run.

## The source is not here

`build.sh` fetches the release from ftp.gnu.org and each of the twenty
patches, checks every one against a pinned SHA-256 (their GPG signatures
were checked against the GNU keyring when they were pinned), and builds
out of tree in `~/m68k/src`. `ports/cross.sh` holds what every GNU-style
port needs to cross-configure for this machine.

Linked dynamically against `/lib/libc.so`, which is why the program on
the disk is about a megabyte rather than three.

## `config.cache`

A cross-configure cannot answer a question by running a program on the
target, so bash's configure guesses — and its guesses are for a system
that is not this one. `config.cache` gives the answers, each with the
reason beside it. The one that matters most:

```
bash_cv_wexitstatus_offset=8
```

The exit status is in bits 8–15 of a wait status, as on Linux. Configure
defaults to 0 when it cannot run a test program, and with that every `$?`
in every script is wrong.

Two entries say what the machine genuinely lacks: `bash_cv_dev_fd=absent`
and `bash_cv_sys_named_pipes=missing`, so process substitution
(`<(cmd)`) is configured out. FAT cannot hold a FIFO and there is no
`/dev/fd`.

## What it needed from the system

bash's own source is unpatched. What it wanted, it wanted from the C
library and the kernel:

- **`%05.2f` printed `02.00`.** picolibc's `printf` was built without
  long-double support, so bash's `printf` builtin — which formats
  through `long double` — lost the field width. The option existed;
  meson only reads its options at setup, so the build directory now
  carries a stamp of them and is wiped when they change
  (`libc/build.sh`).
- **`PC`, `UP` and `BC`.** readline defines termcap's three variables
  itself unless told the termcap library has them, and `libc/termcap`
  does: `-DNEED_EXTERN_PC`.
- **Its own libraries are in `LIBS`.** Overriding `LIBS` on make's
  command line — the usual way to swap a static configure link for a
  dynamic one — wipes out readline, history and glob. The build edits
  the generated Makefiles instead.
- Sessions, `setsid`/`getsid`, `sigaltstack`, `getrusage`, `wait3` with
  a real `rusage`, and `waitpid` with a null status pointer (picolibc
  wrote through it) — all in task 30's list, and all reached first by
  bash.

## Tests

`kernel/bashtest.sh` does two things on the machine:

- **`tests/lang.sh`** — arithmetic, arrays, associative arrays, brace
  expansion, parameter expansion in every form, `case`, command
  substitution, functions, globbing, here-documents, quoting, `trap`,
  `printf`, and `$?` after each — compared **line for line against the
  host's own bash** running the same script. A difference is a
  difference from real bash, not from a table somebody typed.
- **bash's own test suite**, run by `tests/runsuite.sh` on the guest:
  arith, array, braces, case, comsub, func, glob, quote, strip, type and
  varenv by default, each output compared with upstream's `.right` file.
  `BASH_TESTS=all` (or `make bashsuite`) runs all 83, which takes hours —
  one test is minutes of work for a 25 MHz 68040.

The suite needs `THIS_SH`, `BASH` and a `PATH` that finds `./bash`;
upstream's `run-*` scripts assume a Unix with `/bin/bash` already on it.
