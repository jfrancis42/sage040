# Programs

Things that run on the Sage040 rather than being part of it.

```
sage$ hello
hello from a program
  running on Sage040 0.2 (m68040)
  argc = 1
  argv[0] = hello

sage$ cube
cube: rotating wireframe, 640x480, Q12 fixed point
measuring how fast this machine is... 50 fps wanted, 839140 spins per frame
press any key to stop
cube: 538 frames
```

## The naming question

**Programs have no extension.** `CUBE`, not `CUBE.EXE` or `CUBE.BIN`.

That is not a style preference, it follows from how the kernel decides
what is executable. On Linux that is a permission bit, and a FAT16 volume
has no permission bits to set — so the only thing left to look at is the
file itself. `exec.c` reads the first four bytes and expects `\x7fELF`,
then checks the class, the byte order, the type and the machine. A file
that is not an m68k executable is refused by its contents:

```
sage$ BIG.TXT
BIG.TXT: not an executable
```

Which is what Unix has always done — `#!` and ELF magic decide, not the
name — and it means an extension would be decoration that could lie. The
name says what the thing is; the magic number says what format it is in.

## The format

ELF32, big-endian, `EM_68K`, `ET_EXEC`, statically linked at 1 MB. That
is the toolchain's own output, so there is no flattening step and no
private format to document — `make` produces something runnable.

The kernel reads the program headers and loads each `PT_LOAD` segment at
its `p_vaddr`, then zeroes the part the file did not supply, which is
`.bss`. Nothing in `crt0.s` clears it: the loader knows which part of a
segment came from the file and the program does not, and doing it twice
would only hide a loader that had stopped working.

Not supported, deliberately: dynamic linking, relocation, shared
objects, interpreters. Without an MMU a program has to live at a fixed
address anyway.

## Memory

```
0x00000000  kernel: vectors, text, data, bss
0x00100000  program image                  <- USER_BASE
0x002ffff0  program stack, growing down
0x00300000  unused gap
0x003ffff0  kernel supervisor stack
```

The kernel bounds-checks every segment against that window **before**
reading a byte, because with no MMU turned on that check is the only
thing between a mislinked program and the kernel's own memory.

The gap is deliberate: a runaway program stack runs into empty space
rather than straight into the kernel's stack. It is not protection —
nothing stops a program writing anywhere it likes — it just makes the
common accident land somewhere harmless.

`user.ld` and `kernel/exec.h` have to agree about these addresses. They
are written down in both places because the linker needs them at build
time and the loader needs them at run time.

## Running one

The shell looks up anything that is not a builtin:

```
sage$ hello one two          argv arrives intact
sage$ hello -x               exits 1, and the shell says so
sage$ nosuchprogram          command not found
```

Underneath is `spawn()`, which is **not** `execve()`. `execve` replaces
the calling process, and there are no processes here to replace: this
loads a program, runs it, and returns its exit status. When there are
processes it becomes fork + execve + waitpid, the shell keeps the same
shape, and the call underneath changes. Calling it `execve` now would be
a lie that costs nothing today and confuses everyone later.

One program at a time — `exec_spawn()` refuses a nested one, because the
assembly that remembers how to get back has room for a single saved
context.

A program leaves through `exit()`, which unwinds out of however many
frames deep it was, out of the trap it called from, and back into
`exec_spawn` as though the program had simply returned. That is a longjmp
in everything but name, and it is why `exit()` never comes back.

## What a program gets

Two headers: `ulib.h` and the kernel's `uapi.h`, which is the system call
ABI — the numbers, the flags, `struct stat`. It does **not** get
`kernel.h`, `vfs.h`, `dev.h` or anything under `drivers/`. Those describe
the inside of the kernel.

`ulib.c` is the whole C library: the system call stubs, `strlen`,
`memset`, `memcpy`, and enough output to be useful. The stubs are the
same four lines of assembly the kernel uses on its own behalf, because
there is one way in and everything takes it.

### The exception, stated plainly

`cube.c` includes the machine's hardware header and writes to the SM501
directly. There is no framebuffer device yet: a display is neither a byte
stream nor a block device, so it does not fit any class in `dev.h`, and
giving it one is an open item rather than an oversight.

Its text still goes through `write()`, it asks the terminal whether a key
is waiting through `ioctl(FIONREAD)`, and it leaves through `exit()`. Only
the pixels go around the kernel, and the file says so at the top rather
than quietly doing it.

## Pacing

The cube measures the machine instead of guessing at it. There is no
timer to sleep against — the kernel has no tick yet — so a counted spin
is what is left, and the count that gives 50 fps differs by more than an
order of magnitude between hosts.

So it waits for the clock to turn over a second, counts spins until the
next one, and divides. Costs a second at start-up and is right anywhere.
`cube 25` asks for a different rate.

## Building

```bash
make              # every program
make install      # copy them onto the machine's disk
make list         # what is on the disk now
make headers-cube # the ELF the loader will read
```

`make install` uppercases the name, because that is what an 8.3 directory
holds: `cube` becomes `CUBE`. The shell is case-insensitive about it.

| File | |
|------|--|
| `crt0.s` | entry: argc and argv off the stack, `main`, then `exit` |
| `user.ld` | links at 1 MB; no vector table, unlike the kernel |
| `ulib.c` | system call stubs and a very small library |
| `cube.c` | the rotating wireframe cube |
| `hello.c` | the smallest program that proves the facility is general |
