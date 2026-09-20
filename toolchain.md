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
