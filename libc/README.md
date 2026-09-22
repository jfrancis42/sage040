# libc — picolibc for Sage040

Programs can be built against a real C library: stdio, `printf`,
`malloc`, `setjmp`, `qsort`, the maths library, `opendir`, `fork` and
`exec`, signals with `sigaction`, `termios`. It is
[picolibc](https://github.com/picolibc/picolibc) 1.8.12.

```bash
make libc                     # once: fetch, build, install picolibc
make -C libc/test             # a program built against it
```

It installs outside the tree, the way the toolchain and the emulator do:
`~/m68k/sage040-libc`, or `$SAGE_LIBC`.

Two ways to link against it, chosen by `LINK` in `libc.mk`:

- **static** (the default): the library is copied into the program,
  which then runs on any disk.
- **`LINK=dynamic`**: the program uses `/lib/libc.so` through
  `/lib/ld.so` (`../ldso/`), and every process shares one copy of libc's
  text in memory. `make programs` installs both onto `hd.img`.
  `programmer-guide.md`, *Shared libraries*, covers building a library
  of your own.

`build.sh` builds the sources twice for that: once as the static
`libc.a`, and once `-fPIC` for `libc.so`, linked with `libc-so.ld`.
That script is GNU ld's own for `-shared` with three changes, each
marked `SAGE040` and explained where it is; the one worth knowing about
is that libgcc, which is not position independent, goes in the data
segment so the text stays free of relocations.

## The network layer (`net/`)

picolibc has no sockets at all. `net/` is a layer of this project's
own, BSD-licensed like the rest of this directory, built by `build.sh`
into both `libc.a` and `libc.so`:

- **Headers**: `<sys/socket.h>`, `<netinet/in.h>`, `<netinet/tcp.h>`,
  `<arpa/inet.h>` (replacing picolibc's, which had only the byte-order
  macros), `<netdb.h>`, `<sys/un.h>`, `<sys/uio.h>`. Linux's numbers
  throughout -- including `SOCK_NONBLOCK`/`SOCK_CLOEXEC`, which are
  Linux's `O_` values, not picolibc's.
- **The calls**: every socket call, straight to the kernel -- except
  `SO_RCVTIMEO`/`SO_SNDTIMEO`, converted, because picolibc's
  `struct timeval` has a 64-bit `tv_sec` and the kernel's 32. `readv`
  and `writev` are loops (the kernel has neither), not atomic.
- **`inet_aton`, `inet_addr`, `inet_ntoa`, `inet_pton`, `inet_ntop`**,
  IPv4.
- **The resolver**: `getaddrinfo`, `freeaddrinfo`, `gai_strerror`,
  `getnameinfo` (numeric: there is no reverse DNS), `gethostbyname`,
  `getservbyname`/`getservbyport` (a small built-in table), `h_errno`.
  Numeric names, then `/etc/hosts`, then `localhost`, then a cache, then
  DNS to `/etc/resolv.conf`'s servers or DHCP's. Answers are cached
  per process for their TTL; "no such name" for 30 seconds. IPv4 only:
  `AF_INET6` is `EAI_FAMILY`.

`test/inettest.c` exercises all of it; `kernel/dnstest.sh` runs it
against a DNS server on the host.

## Why this was a small port

picolibc already runs on Linux. `libos/linux` is a POSIX layer over
Linux's system calls, and it translates picolibc's own errno and signal
numbers (which are newlib's, BSD-flavoured) to and from Linux's. It has
backends for arm, aarch64 and x86.

This kernel's system call interface is Linux/m68k's, on purpose: the
numbers, the calling convention, the structures. So the port is:

| | |
|---|---|
| `picolibc/libos/linux/machine/m68k/` | the m68k backend: Linux/m68k's constants and structure layouts, a `syscall()` in assembly, and the POSIX calls picolibc's Linux layer lacks everywhere (`pause`, `usleep`, `select`, `flock`, `ftruncate`, `truncate`) |
| `patches/` | fixes to picolibc itself, for bugs that are not about m68k |
| `build.sh` | fetch, verify, lay the backend over the release, apply the patches, build |
| `termcap/` | termcap with the one terminal compiled in -- a VT102 -- and the size asked of the terminal; BSD-licensed, since it is linked into programs that are not GPL |
| `crt0.s`, `sage040.ld`, `libc.mk` | how a program starts, is laid out and is built |

and, on the kernel's side, the calls picolibc makes that this system did
not have: `statx`, `getdents64`, the `rt_sig*` family with `SA_SIGINFO`,
`openat` and the other `*at` calls, `pipe2`, `dup3`, `wait4`,
`clock_gettime`, `_llseek`, `prlimit64`, `getrandom`, `TCGETS2`, the uid
calls. They are in `kernel/syslinux.c`, and they are Linux's -- so they
serve any Linux-targeted C library, not only this one.

## The m68k backend's headers

The `linux/*.h` files are generated upstream by compiling small programs
against Linux's headers with a glibc cross compiler and running them
under qemu-user. Neither exists here, so the m68k set was made from the
i686 one (the other 32-bit set) and every value that differs on m68k was
changed by hand against Linux's own m68k sources:

- `linux-syscall.h`: the whole table, from
  `arch/m68k/kernel/syscalls/syscall.tbl` -- the same list
  `kernel/linux-m68k-syscalls.txt` holds, which `kernel/abicheck.sh`
  checks the kernel against on every build.
- `linux-fcntl.h`: m68k's `O_DIRECTORY` (0x4000), `O_NOFOLLOW`,
  `O_DIRECT` and `O_LARGEFILE`, which are not the generic values.
- `linux-poll.h`: m68k's `POLLWRNORM` is `POLLOUT`, and `POLLWRBAND` 256.
- `linux-signal.h`: `LINUX_SA_RESTORER` is 0. m68k defines no
  `SA_RESTORER`, and the kernel writes its own return trampoline into
  each signal frame, which is what makes `SA_SIGINFO` handlers return
  through `rt_sigreturn` and ordinary ones through `sigreturn`.

The structure headers use explicit byte offsets (`__adjust_N` padding
arrays), so they come out the same under m68k's two-byte alignment of
`int` as under i686's four.

## Differences from glibc a port will meet

- **`environ` is declared nowhere.** Declare it yourself
  (`extern char **environ;`), as POSIX has traditionally required.
- **`CLOCK_MONOTONIC` needs `_GNU_SOURCE`.** picolibc only claims
  `_POSIX_MONOTONIC_CLOCK` for RTEMS, so under plain POSIX the constant
  is hidden where glibc shows it.
- **`statvfs` fails with `ENOSYS`**: the kernel has no `statfs64`.
- **`ioctl()` knows only `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCLINUX` and
  `FIONREAD`**: picolibc translates its own request numbers to Linux's
  and refuses the rest with `EINVAL`. `FIONREAD`, and `struct winsize`
  being visible from `<sys/ioctl.h>`, come from `patches/`.
- **termcap is `-ltermcap` and `<termcap.h>`**, not ncurses: change a
  program's `#include <curses.h>` and `<term.h>` if it only wanted
  termcap from them.
- **One thread per process**, so picolibc is built without thread-local
  storage or locking, and `errno` is an ordinary global.
- **A function's address is canonical**, as C requires: a dynamic
  program's `&printf` is the program's own PLT entry, and `ld.so` hands
  the same address to any library that asks. It is not an address inside
  libc.so.

## Licence

picolibc is BSD-licensed. Everything under `picolibc/`, `patches/` and `termcap/`
is BSD-3-Clause like the files around it, so it could go upstream as it
is; the rest of this directory is GPL-3.0-or-later like the project.
