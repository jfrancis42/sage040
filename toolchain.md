# Building the m68k cross toolchain

Everything here targets **`m68k-elf`** — a bare-metal target with no operating
system and no C library. That is the right shape for writing a kernel: you get
the compiler, assembler, linker and debugger, and nothing that assumes a host
underneath.

Versions this tree is built and tested with:

| Tool | Version |
|------|---------|
| binutils (`as`, `ld`, `objdump`, …) | 2.45 |
| GCC | 15.2.0 |
| GDB | 17.1 |

Nothing depends on those exact versions; anything reasonably recent works.

---

## Quickest route

Some distributions package a ready-made cross toolchain. On Arch, the AUR has
`m68k-elf-binutils`, `m68k-elf-gcc` and `m68k-elf-gdb`. If those install
cleanly, skip to [Using it](#using-it).

Otherwise building from source takes about twenty minutes and is completely
routine.

---

## Building from source

Pick a prefix and put it on `PATH`. Everything below assumes:

```bash
export PREFIX="$HOME/m68k/install"
export PATH="$PATH:$PREFIX/bin"
export TARGET=m68k-elf
mkdir -p "$PREFIX" ~/m68k/src && cd ~/m68k/src
```

Build dependencies, which most systems already have: `gmp`, `mpfr`, `libmpc`,
`texinfo`, `bison`, `flex`, plus a working host compiler and `make`.

### 1. binutils

```bash
curl -LO https://ftp.gnu.org/gnu/binutils/binutils-2.45.tar.xz
tar xf binutils-2.45.tar.xz
mkdir build-binutils && cd build-binutils
../binutils-2.45/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --disable-werror
make -j"$(nproc)" && make install
cd ..
```

### 2. GCC

Only the C compiler and `libgcc` are needed. `--without-headers` tells GCC
there is no target libc, which is exactly the situation.

```bash
curl -LO https://ftp.gnu.org/gnu/gcc/gcc-15.2.0/gcc-15.2.0.tar.xz
tar xf gcc-15.2.0.tar.xz
cd gcc-15.2.0 && ./contrib/download_prerequisites && cd ..
mkdir build-gcc && cd build-gcc
../gcc-15.2.0/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$(nproc)" all-gcc all-target-libgcc
make install-gcc install-target-libgcc
cd ..
```

### 3. GDB

```bash
curl -LO https://ftp.gnu.org/gnu/gdb/gdb-17.1.tar.xz
tar xf gdb-17.1.tar.xz
mkdir build-gdb && cd build-gdb
../gdb-17.1/configure --target=$TARGET --prefix="$PREFIX" \
    --with-python=/usr/bin/python3 --disable-nls --disable-werror
make -j"$(nproc)" all-gdb && make install-gdb
```

GDB is worth building even though nothing needs it to run: `target remote`
into a running machine is how you look at a kernel that has stopped
somewhere unexpected. See §13 of the programmer's guide.

`--with-python` is optional but gives you pretty-printers and scripting. If
GDB is linked against a Python that later gets upgraded out from under it, it
fails at startup with a missing `libpython3.x.so` — rebuild it against the
current interpreter rather than hunting for the old library.

### 4. Check it

```bash
m68k-elf-gcc --version
m68k-elf-ld --version
m68k-elf-gdb --version
```

---

## Using it

### Two sets of flags

The tree builds two different kinds of thing, and they differ in one
place that matters: the include path.

**Bare metal** — the tests, the boot ROM, `cube/`, and the kernel itself.
These get the machine's hardware header and own every register.

**Programs** — everything in `apps/` and `system/`, the ports, and
anything built against picolibc. These get `kernel/uapi.h`, the system
call ABI, and deliberately *not* the hardware header: `lib/program.mk`
leaves `../tests` off the include path, so a program cannot reach a chip
by adding an `#include`. The MMU stops it at run time; the build stops it
at compile time, which turns it into a decision rather than a slip.

The kernel additionally defines `-DSAGE040_NO_TESTLIB`, which fences off
the test support library's declarations in `sage040.h` — names like
`uart_rx_ready` and `mfp_clear_pending` are exactly what a driver wants to
call its own helpers.

### Compiler flags

```make
CPUFLAGS := -mcpu=68040
CFLAGS   := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
            -Wall -Wextra -fno-builtin -fno-stack-protector -I.
LDFLAGS  := $(CPUFLAGS) -ffreestanding -nostdlib -T sage040.ld \
            -Wl,--build-id=none -Wl,--no-warn-rwx-segments
```

- `-ffreestanding -nostdlib -nostdinc` — no host headers, no startup files, no
  libc. You supply `_start` yourself.
- `-fno-builtin` stops GCC turning your loops back into `memcpy` calls you have
  not written. It will still emit `memcpy` and `memset` for struct copies, so
  supply those two eventually regardless.
- `-mcpu=68040` selects the 68040 and implies hardware floating point. The
  freestanding headers you *do* get are `stddef.h`, `stdint.h`, `limits.h`,
  `stdbool.h` and `stdarg.h`.
- `--no-warn-rwx-segments` silences a warning that is meaningless for a
  single-segment bare-metal image.

### Verifying the FPU is real

```bash
echo 'float f(float a, float b){return a*b;}' \
  | m68k-elf-gcc -mcpu=68040 -ffreestanding -O2 -S -xc - -o -
```

You should see `fsmul.s` and no calls to `__mulsf3`. If you get the latter,
the compiler has fallen back to soft float.

### Useful binutils

```bash
m68k-elf-objdump -d kernel.elf     # disassemble
m68k-elf-nm      kernel.elf        # symbols
m68k-elf-size    kernel.elf        # text/data/bss
m68k-elf-readelf -a kernel.elf     # headers, sections, segments
m68k-elf-objcopy -O binary k.elf k.bin
```

### Debugging

QEMU exposes a GDB stub. `-S` freezes the machine at the reset vector so you
can set breakpoints before anything runs.

```bash
qemu-system-m68k -M sage040 -cpu m68040 -m 4 -kernel kernel.elf \
    -serial file:out.txt -display none -S -gdb tcp::1234 &

m68k-elf-gdb kernel.elf \
    -ex 'target remote :1234' \
    -ex 'break main' \
    -ex 'continue'
```

| Command | |
|---|---|
| `info registers` | D/A registers, PC, SR |
| `info all-registers` | adds the FPU registers |
| `x/20i $pc` | disassemble at the program counter |
| `stepi` / `nexti` | single-step one instruction |
| `x/8xw 0xff300000` | peek at a device |

`p $vbr` returns `void` — GDB's m68k description does not expose the control
registers. Read them from the guest instead.

### Assembler

GCC drives `m68k-elf-as` (GNU/AT&T syntax) for `.s` files, which is what this
tree uses. If you prefer Motorola syntax, `vasm` (`vasmm68k_mot`) is a good
standalone assembler; the raw-binary output needs wrapping in an ELF for
QEMU's `-kernel`, which `boot/mkelf.py` shows how to do in a few lines.

---

## The toolchain that runs ON the machine

Everything above builds tools that run on a workstation and produce code
for the Sage040. This section is about the other direction: tools that
run on the Sage040 itself, so the machine can build its own programs --
and, eventually, its own kernel -- without another computer.

**It is still built here.** Nothing is compiled inside the emulator; the
emulator runs the result, which is the test rather than the build. The
technique is a **Canadian cross**, where three machines are named
separately:

| | |
|---|---|
| `--build` | the workstation doing the compiling |
| `--host` | where the finished tool runs — the Sage040 |
| `--target` | what the finished tool generates code for — also the Sage040 |

`--host` equal to `--target` is what makes the result a *native*
toolchain rather than a cross compiler that happens to run there.

The ports build it in this order, and the order is forced:

```bash
make -C ports/binutils install     # as, ld, ar, nm, objdump, strip, ...
make -C libc install               # the C library at /usr on the machine
make -C ports/gmp install          # gcc's arithmetic
make -C ports/mpfr install
make -C ports/mpc install
make -C ports/libstdcxx install    # needs a cross g++; see below
make -C ports/gcc install
```

### Why there is a second cross compiler

GCC 15 is written in C++. A compiler that runs on the machine therefore
needs a **C++ runtime that runs on the machine**, which means libstdc++
must be built for the target before the native gcc can be built at all.
And libstdc++ needs a C++ compiler targeting m68k, which the toolchain
above does not include -- it is configured `--enable-languages=c`.

So there is a second cross gcc, built with `c,c++`, into a prefix of its
own:

```bash
mkdir build-gcc-cxx && cd build-gcc-cxx
../gcc-15.2.0/configure --target=m68k-elf \
    --prefix="$HOME/m68k/install-cxx" \
    --disable-nls --enable-languages=c,c++ --without-headers \
    --with-gnu-as --with-gnu-ld --disable-multilib --with-cpu=68040 \
    CXX="g++ -std=gnu++17" CXX_FOR_BUILD="g++ -std=gnu++17"
make -j4 all-gcc && make install-gcc
```

**A separate prefix on purpose.** `~/m68k/install` is the C compiler
every other thing in this tree depends on, and it is not worth putting
at risk to add a language. The two are the same gcc version for the same
target, so an object from one links against an object from the other.

`CXX="g++ -std=gnu++17"` because a host g++ that defaults to C++20 --
GCC 16 does -- compiles gcc 15's own `libcody` wrongly: `u8""` literals
became `char8_t` in C++20 and libcody predates it.

`make install-gcc` installs the compiler and **not** the target
libgcc, so the new prefix has no `libgcc.a`. The ports pass
`-B` at the existing one rather than build a second copy; same version,
same target, and libgcc does not depend on which front ends were built.

### Things that bite in a Canadian cross

**Flags belong in `$CC`, not only in CFLAGS.** A package like binutils
configures a dozen subdirectories of its own and passes CFLAGS down to
each but not CPPFLAGS -- and autoconf's preprocessor-only tests run as
`$CPP $CPPFLAGS` with no CFLAGS anywhere near them. Put the
`-nostdinc -isystem ...` in CC and it is in all three: compiling,
preprocessing and linking.

**`libc/sage040.specs` is what makes a plain link work.** Without it,
every link needs eleven flags, and a configure script that writes
`${CC} -o conftest ${CFLAGS} ${LDFLAGS} conftest.c` and no `${LIBS}` --
binutils' own does -- cannot pass. With it, `gcc hello.c -o hello` links
a dynamic program against `/lib/libc.so` and `gcc -static` a static one.

**Cache variables have to be exported**, not passed as configure
arguments: a subdirectory's configure is run later, by `make`, with a
cache file of its own. `ac_cv_tls=none` is needed because this system
has no thread-local storage and binutils tests for it by *compiling*
`thread_local int x;` and never linking it.

**Tools under the target's own name.** `--target=m68k-unknown-elf` makes
gcc's build look for `m68k-unknown-elf-gcc` and `-as` when it needs to
compile something for the target, and the cross tools here are installed
as `m68k-elf-*`. `ports/gcc/build.sh` makes a directory of symlinks
under the expected names and puts it on PATH; without it the build links
`xgcc` and then dies on `m68k-unknown-elf-gcc: command not found`.

**`all-host`, not `all`.** `all` goes on to build the target libraries --
libgcc, libstdc++ -- for a target whose C library is already installed,
and rebuilds them in the wrong place. `all-host` stops at the programs,
which is what a native toolchain is.

**`--without-isl`.** isl is configured before gmp is anywhere it can be
found, so the configure fails on a library the build itself is about to
produce. gcc needs it only for Graphite loop optimisations, which
nothing here asks for.

**gcc's bundled gettext has to go** (`ports/gcc/patches/01`). gcc 15
carries a copy of gettext's gnulib and configures it for the host it is
built ON, not the one it is built FOR; it then fails on the difference.
gcc does not need message catalogues to compile C.

**`extern "C"` in every network header.** This was a real bug rather
than a build inconvenience: `libc/net/include/*.h` had no
`_BEGIN_STD_C`, so a C++ translation unit got C++ linkage for
`htons` and friends and the link failed on symbols that existed. Every
one of the seven has the guards now.

### That it is the same compiler

The point of a native toolchain is not that it runs but that it is
**the same compiler**. `kernel/nativetest.sh` (15 checks) compiles on
the machine and compares: the object files it produces, disassembled,
are the cross compiler's -- same instructions, same order, for the same
source at the same optimisation level.

One genuine difference is worth knowing: **the raw object files are not
byte-identical**. The native assembler leaves uninitialised bytes in
section padding where the cross one writes zeroes, so `cmp` on two
`.o` files fails while every section a tool reads is the same. Compare
disassembly, or the linked output, not the bytes.

**It needs a bigger machine than the default.** gcc compiling anything
real wants more than 64 MB, so the suite runs with `NATIVE_RAM_MB=256`.
The machine sizes RAM at run time, so this is an emulator flag and not
a rebuild.

### What the machine needs on its own disk

A compiler running on the Sage040 looks for headers and libraries on the
Sage040. `make -C libc install` puts them there:

| | |
|---|---|
| `/usr/include` | picolibc's headers, and the network ones |
| `/usr/lib` | `libc.a`, `libc.so`, `liblinux.a`, `libm.a`, `libgcc.a` |
| `/usr/lib/crt0.o`, `crt0-dyn.o` | where a program starts |
| `/usr/lib/sage040.ld` | how a static program is laid out |
| `/usr/bin` | the tools themselves; `/usr/bin` is on the shell's PATH |
