# CPython on SuckOS

**CPython 3.14.7**, cross-built against picolibc, statically linked, with
every extension module built in and the standard library on the disk.

```bash
make libc                     # once: picolibc
make -C ports/python          # fetch, patch, cross-configure, build
make -C ports/python install  # onto hd.img: /usr/local/bin/python3 and the library
make pytest                   # the suite, on the machine
```

```
/$ python3 -V
Python 3.14.7
/$ python3 -c "import math; print(sum(math.exp(i/8) for i in range(16)))"
47.98445608469837
```

## The source is not here

`build.sh` fetches the release from python.org, checks it against a
pinned SHA-256, applies the four patches in `patches/`, and builds out of
tree in `~/m68k/src`. Nothing built lands in this directory.

**It needs a Python to build Python**: the build runs the interpreter on
the host to freeze the startup modules and compile the standard library,
and it must be the same version. `BUILD_PYTHON` says which, and the
version here is pinned to what the host has.

## What is different about this port

- **Every module is built in.** There is no `dlopen` on this machine, so
  `MODULE_BUILDTYPE=static` links the lot into one 6.5 MB executable.
  What is not in it is not there at all.
- **No compiler thread-local storage.** m68k has no thread register, so
  `__thread` becomes a call to `__m68k_read_tp` that a C library can only
  answer by keeping an ELF TLS block per thread. `patches/02` and
  `patches/03` take the thread-key path CPython's own comments describe.
- **No mimalloc**, for the same reason; pymalloc is used instead.
- **`sys.platform` is `linux`.** CPython's configure knows a fixed list
  of host systems and refuses anything else, and of the ones it knows
  linux is the true answer: this kernel's system call numbers, calling
  convention and errnos are Linux's. `__linux__` stays undefined, so code
  that tests for it directly compiles out.
- **The `.pyc` files are hash-based and unchecked.** A normal `.pyc`
  carries its source's size and modification time, and the times on a FAT
  filesystem are whatever mcopy wrote -- so every module looked stale and
  the machine recompiled the standard library at every import.
  `statistics` alone took minutes.

## What is not built

`_ctypes` (no libffi and no dlopen), `ssl`, `sqlite3`, `bz2`, `lzma`,
`zstd`, `tkinter`, and `readline` -- the last only until the library is
ported, at which point this is rebuilt, because an interactive
interpreter with no line editing is the one obvious thing missing.

## What it found

Running a large, careful C program on a big-endian 32-bit machine finds
things. Each of these is a patch in `libc/patches/` or
`ports/python/patches/`, with the reasoning at its head:

- **`malloc` returned two-byte-aligned memory.** The m68k ABI aligns even
  a double to 2, so picolibc's malloc was conforming -- and CPython's
  garbage collector, which keeps flags in the low two bits of every
  object's list pointers, corrupted itself on its first collection.
  `python3 -c "print(1+1)"` died with a bus error at a wild address.
- **The static linker script never placed `.got`.** ld put it where it
  liked and defined `_GLOBAL_OFFSET_TABLE_` from that, so every address
  fetched through the GOT came back null. Only a static program built
  `-fPIC` notices, and CPython is the first one here.
- **MD5 was wrong.** CPython's bundled HACL* defines `htole64` on a
  big-endian machine as two `htobe32` calls, which is the identity --
  so it exchanged the halves of the word and reversed nothing inside
  them. The digest of the empty string was right, because zero survives
  any permutation, and every other digest was wrong. SHA-1 and SHA-2,
  being big-endian formats, were unaffected.
- Six more in picolibc: `SSIZE_MAX` unusable in `#if`, `struct rusage`
  with two fields instead of sixteen, `ioctl` declared non-variadic,
  `<sys/types.h>` not pulling in `<sys/select.h>`, `RLIMIT_*` numbers
  that were not the kernel's, and `fcntl(F_SETFL)` passing picolibc's
  `O_NONBLOCK` to a kernel that means something else by it.

## Tests

`kernel/pytest.sh` runs a script on the machine and asks the HOST's
Python 3.14 the same questions: the version, the byte order, the size of
a pointer, big integers, `0.1 + 0.2` to the last bit, sorting, dict
comprehensions, f-strings, 35 standard-library imports off the disk,
hashes, zlib, files, threads with a lock, time zones, and a subprocess.

Two answers are allowed to differ, and the suite says why: `sys.maxsize`,
because this is a 32-bit machine, and a sum of exponentials, because the
68040 computes `exp` in 80-bit extended precision and rounds once while
an x86-64 host works in doubles. Both are correct; they differ in the
last place.
