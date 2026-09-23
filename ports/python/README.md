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
  carries its source's size and modification time, and the times on the
  image are whatever the copy onto it wrote -- so every module looked
  stale and the machine recompiled the standard library at every import.
  `statistics` alone took minutes.

## The libraries it is built against

Seven ports, each fetched at a pinned version, built into `~/m68k/src`
with nothing landing in this tree, and wired into `make pylibs` --
which `make python` depends on. **90 built-in modules**, where a CPython
built without them has 87.

| port | what it gives CPython | on the disk |
|------|----------------------|-------------|
| **bzip2** 1.0.8 | `bz2` | `/bin/bzip2` |
| **xz** 5.6.3 | `lzma` | `/bin/xz` |
| **zstd** 1.5.7 | `compression.zstd`, new in 3.14 | `/bin/zstd` |
| **SQLite** 3.53.4 | `sqlite3` | `/bin/sqlite3` |
| **OpenSSL** 3.5.4 | `ssl`, and a `hashlib` whose digests come from OpenSSL | `/bin/openssl` |
| **readline** 8.3 | line editing and history at the interactive prompt | a library only |
| **libffi** 3.5.2 | `ctypes`, as far as it goes without `dlopen` | a library only, with `fficheck` |

`kernel/pylibtest.sh` checks them, and **every stream crosses the host
boundary in both directions**: the machine compresses and the host
decompresses, and the reverse. A compressor tested against its own
output is self-consistently wrong at best, and these are byte streams
with a defined byte order on a big-endian machine. The digests are
compared with coreutils' `md5sum`, `sha1sum`, `sha256sum` and
`sha512sum` -- a different implementation on a different CPU -- and with
the published SHA-256 of the empty string, which is owed to nothing on
this host at all.

## What is still not built

- **`_ctypes`**, and **not for want of libffi**, which is built and
  demonstrably works. `_ctypes.c` includes `<dlfcn.h>` and opens
  libraries by name at run time; this loader has no `dlopen`. A missing
  loader feature, not a missing library.
- **`_multiprocessing`** and **`_posixshmem`**, both wanting POSIX
  shared memory.
- **`tkinter`**.

**`xz` at its default preset will not run here.** LZMA's memory use
follows its dictionary size, and `-6` -- which plain `xz` uses -- wants
about 94 MB on a machine with 64. It fails cleanly with "Not enough
space" and exit 1 rather than crashing, and the suite checks that it
does, so the low preset used elsewhere is not mistaken for timidity.
`-1` needs about 9 MB.

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
comprehensions, f-strings, 36 standard-library imports off the disk,
hashes, zlib, files, threads with a lock, time zones, and a subprocess.

Two answers are allowed to differ, and the suite says why: `sys.maxsize`,
because this is a 32-bit machine, and a sum of exponentials, because the
68040 computes `exp` in 80-bit extended precision and rounds once while
an x86-64 host works in doubles. Both are correct; they differ in the
last place.
