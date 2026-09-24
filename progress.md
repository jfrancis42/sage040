# Todo

What is **not** done. Everything that is finished is described as state,
where somebody will meet it:

| | |
|---|---|
| [`os.md`](os.md) | the system -- tasks, memory, files, the terminal, the network |
| [`design.md`](design.md) | the machine, and the decisions behind it |
| [`README.md`](README.md) | how to build it and run it |
| [`toolchain.md`](toolchain.md) | the compilers, cross and native |
| [`libc/README.md`](libc/README.md) | the C library, and what a port will meet |
| [`ports/`](ports/) | each ported program: a README, or the reasoning at the head of its `build.sh` |

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk -- both can be increased
and have been.

A task is crossed off when its tests pass, and every task ships with
them: a check that could pass vacuously gets a negative control -- a
deliberate break that must make it fail.

---

## State of things, 2026-09-23

Everything broken, unfinished, unverified or waiting on a decision, in
one place, so that none of it has to be discovered by reading the rest
of this file. Dated, because it goes stale.

### Broken, or not working

**Nothing known.**

### Unverified -- believed working, not proven

| | |
|---|---|
| **The suites' tolerance of load** | Most suites `sleep` a fixed time and assume the machine is ready. Under load it is not, and the failure looks like the thing being tested: `sshtest` lost one of ten connections in a back-to-back batch and passed all ten on an idle machine. `sshtest` polls until the machine answers; the others do not. |

### What the suites say

Every suite in the tree passes, on ext2:

`tests/` (12) · `fsimgtest` (37) · `fstest` (65) · `fattest` (10) ·
`fscktest` (23) · `devtest` (36) · `sotest` (119) · `vmtest` ·
`edittest` · `usertest` · `threadtest` (52) · `ptytest` (34) ·
`vttest` (95) · `nettest` (19) · `tcptest` (24) · `lotest` ·
`crontest` · `dnstest` (57) · `pagetest` · `logtest` · `cryptotest` ·
`uemacstest` · `vitest` · `lesstest` · `curstest` · `sedtest` ·
`awktest` · `apitest` · `greptest` · `sbasetest` · `dftest` ·
`libctest` (246) · `nativetest` (15) · `qemutest` (8) · `pylibtest` ·
`sshtest` · `pytest`

`bashtest` is the exception at 13 of 15, for the two reasons below.

### Needs a decision -- yours, not mine

| | |
|---|---|
| **A Lisp: CLISP or ECL** | Both are possible now, and the choice is real rather than a formality. See below. |
| **Permission enforcement (36)** | The disk records an owner, a group and a mode, and a file a user creates belongs to that user. Nothing checks any of it. See below. |
| **Symlinks** | ext2 holds them and `stat` reports `S_IFLNK` rather than mistaking one for a short file, but nothing creates or follows one. That is VFS and system-call work -- `symlink`, `readlink`, `O_NOFOLLOW`, and following during a path walk -- not filesystem work. |

### Open, and nothing is blocking them

- **MMU: use the M (modified) bit when evicting. ATTEMPTED AND
  REVERTED -- read this before trying again.**

  The idea is sound: `evict()` writes every page to swap
  unconditionally, and a page read in from swap and never written
  since has an identical copy on disk already, so it could be DROPPED
  for nothing. The hardware records "was this written" in the M bit at
  no cost.

  It was built -- a per-frame swap-slot association in pmm, kept across
  a page-in, consulted at eviction -- and it corrupted memory. Six
  separate defects were found and fixed and `pagetest forkswap` still
  failed, so the whole thing was taken back out. What was learned is
  worth more than the code was:

  **1. THE KERNEL WRITES USER PAGES BY PHYSICAL ADDRESS.**
  `uaccess_chunk()` calls `vm_translate()` and memcpy's through the
  kernel's identity map, so a `read(2)` filling a program's buffer sets
  no M bit on that program's descriptor. Anything that ever trusts M
  has to account for this. It is a landmine independent of swap.

  **2. M IS PER-DESCRIPTOR; A REMEMBERED SLOT IS PER-FRAME.** For a
  page two address spaces share, one space's M says nothing about what
  the other has done. Restricting the optimisation to refcount-1
  non-COW pages was still not enough to make forkswap pass.

  **3. HOLDING A SLOT PER RESIDENT PAGE SHRINKS THE SWAP FILE.** A
  resident page and its slot hold the same data, so a program needing
  most of swap fails with "out of memory" on a machine with plenty. The
  association has to be reclaimable.

  **4. A STALE ASSOCIATION ON A REUSED FRAME IS CORRUPTION, NOT A
  LEAK.** Clear it where the frame is handed out, not only where it is
  freed.

  **5. NEVER WRITE OVER THE SLOT A PAGE CAME FROM.** Another address
  space may still have a swapped descriptor pointing at it, and the
  slot's refcount does not always say so. pagetest says this in its own
  words: "the slot it came from is still the child's, and must not be
  the one reused."

  Anyone picking this up should start by making `pagetest forkswap`
  pass, in a SINGLE run of the suite -- two copies of one suite share
  `/tmp/scratch/hd-page.img` and produce results that look like kernel
  bugs.
- **MMU: turn the CACHES on. DONE.** `kernel/cache.c` says it plainly --
  "the caches are OFF: nothing writes CACR" -- so 4 KB of instruction
  and 4 KB of data cache sit idle. The groundwork is already right:
  descriptors carry CM copyback for RAM and non-cachable for I/O, the
  transparent-translation registers mark the I/O and framebuffer
  windows non-cachable, and `cacheflush(2)` issues `cpusha`.
  **THIS CANNOT BE VERIFIED HERE.** QEMU ignores the CM bits and
  decodes `cinv` and `cpush` as no-ops, so on the emulator a correct
  cache setup and a broken one are indistinguishable and there is no
  speedup to measure either. It is for real hardware, and it is
  correct by inspection rather than by test -- which is worth saying
  wherever it is switched on.
- **MMU: PTEST and MMUSR instead of walking the tables. LOOKED AT AND
  REJECTED, with the reason.** PTEST does the table search in hardware
  and reports the result in MMUSR, which sounded like a way to delete
  `vm_fault()`'s software walk.
  It cannot be, and the reason is this kernel's own design: demand
  paging keeps its state -- lazy, in-swap (with the SLOT NUMBER in bits
  31..12), copy-on-write, PROT_NONE -- in INVALID descriptors, because
  the MMU ignores every bit but the type field on those and they fault
  exactly as an unmapped page would. PTEST stops at an invalid
  descriptor and reports nothing about it: in QEMU's implementation,
  `if (!M68K_PDT_VALID(next)) return -1;` returns before MMUSR is
  filled in at all, and a real 68040 likewise reports R=0 and no
  descriptor bits. So PTEST can say "not resident", which vm_fault
  already knows from having been called, and cannot say WHICH of the
  four cases it is or what slot to read.
  It would still serve as a diagnostic -- a way to ask the hardware
  what it thinks of an address, independently of the walk -- and that
  is the only thing worth building it for.
- **MMU: the G (global) bit.** Global ATC entries survive a selective
  `pflush`; this kernel uses `pflusha` -- flush everything -- on every
  mapping change, COW fault and reclaim sweep, which throws away the
  kernel's own entries every time. The 68040's ATCs hold 64 entries
  between them, so that is real refill traffic.
  **THE BIT LAYOUT, CHECKED.** On a 68040 page descriptor bit 8 is U0,
  bit 9 is U1 and **bit 10 is G** -- so `DESC_SW_LAZY`, which is 0x400,
  sits exactly on the global bit, and `DESC_SW_SWAP` (0x200) on U1.
  (Taken from QEMU's own `M68K_MMU_G_040` and friends, which match the
  manual.)
  Nothing is broken by that and nothing needs moving: SW_LAZY is only
  ever read on an INVALID descriptor and G only means anything on a
  RESIDENT one, so the two never coexist. Setting G on resident kernel
  pages is safe as the numbering stands.
  **What makes this NOT worth doing yet** is the other end: the gain
  only arrives if `pflusha` -- flush everything -- is replaced by
  selective flushes at each of its call sites, and every one of those
  needs its own argument about what may still be cached afterwards. A
  wrong one is not a crash, it is a stale translation used later
  somewhere unrelated, which CLAUDE.md already records as close to
  undebuggable. And the benefit cannot be measured here: it is ATC
  refill traffic on hardware that does not exist yet.

- **ssh by password.** dropbear authenticates by public key; it has to
  be pointed at `/etc/shadow` and told to use `crypt(3)`, which exists
  now (`$6$` SHA-512, `libc/picolibc/.../crypt.c`). The console asks
  for a password already -- `/bin/login` -- so this is the same check
  reached from a different direction, and the pieces are all present.
- **Symbolic links.** ext2 stores them and `stat` reports `S_IFLNK`
  rather than mistaking one for a short file, but nothing creates or
  follows one. That is VFS and system-call work -- `symlink`,
  `readlink`, `O_NOFOLLOW`, and following during a path walk, with a
  depth limit so a loop is ELOOP rather than a hung kernel -- not
  filesystem work. A fast symlink (the target in the inode, under 60
  bytes) and a slow one (in a block) are both ext2 and both have to be
  read.
- **Hard links.** `link(2)` and `linkat(2)` answer -EPERM today, from
  when the filesystem was FAT and could not have them. ext2 can: a
  second directory entry pointing at the same inode, with `i_links_count`
  raised -- and unlink already has to decrement it and free the inode
  only at zero, which is what `fsck_inode_live()` reads. The awkward
  part is not the making but everything that assumed one name per
  inode.

- **The POSIX gaps that are left** (30): FIFOs, `/dev/fd`, a listable
  `/dev`, and `diff`. Detailed below.
- **Permission enforcement** (36), above.
- **A Lisp** (48), above.
- **libatomic** (50). Nothing has asked for it; the reasoning is below.
- **gdb, native.** It is C++ and libstdc++ exists, so the remaining
  obstacle is `ptrace` in the kernel -- which does not exist at all.
  Without it a debugger cannot stop, inspect or step anything.
- **Regression tests** (23), which is ongoing and never finished.
- **Three per-port workarounds can come out**, now that the gcc
  integer-type fix has removed the reason for them:
  `ports/binutils/patches/01` and `02`, `ports/openssl/patches/01`, and
  `ZSTD_LEGACY_SUPPORT=0` in `ports/zstd/build.sh`, which was set to 0
  only because of it. One at a time, each with a rebuild to confirm.
  Nothing is wrong as it stands, which is why this is not urgent.
- **bash passes 13 of bash's own 15 cases.** `func` wants process
  substitution -- FIFOs and `/dev/fd`, above. `glob` wants the `locale`
  command and a `zh_TW.big5` locale, and says so itself in a warning.

### Known limitations, accepted rather than outstanding

These are properties of the machine, written up in `design.md` and
`os.md`. They are here so they are not mistaken for bugs.

- **No `dlopen`**, so no `ctypes` -- and that is the loader's missing
  feature, not libffi's. libffi is built and demonstrably works.
- **No thread-local storage.** The 68040 has no thread pointer
  register; `__thread` needs `PT_TLS` in `ld.so`, a per-thread block,
  and `__m68k_read_tp`, all three.
- **No `crypt(3)`**, so no ssh password authentication. Public keys
  work.
- **Users protect nothing** -- the disk records owners and modes;
  nothing enforces them.
- **Object files built on the machine are not byte-reproducible.** The
  native assembler leaves uninitialised bytes in section padding where
  the cross one leaves zeroes. Every section a tool reads is identical.
- **`xz` at its default preset will not run**: `-6` wants about 94 MB
  and the machine has 64. `-1` works.
- **A fault's own signal cannot be caught**, the 68040's access-fault
  frame not being redirectable in place.

### Where the traps are written down

Facts about this machine that cost time to learn live where somebody
will meet them again, not in a list here: properties of the hardware in
`design.md`, properties of the system in `os.md`, and the reason a
particular line of code is the way it is in the comment above that
line.

---

## The open tasks, in detail

### 30. The POSIX gaps that are left

Found while porting awk, sed, grep, bash and sbase, and worth closing
whether or not anything needs them yet. Pseudo-terminals and
`PATH_MAX` were the rest of this task and are done.

- **FIFOs.** A named pipe has to live in the VFS: ext2 could hold the
  node, but nothing creates a device node or a FIFO on disk.
- **`/dev/fd`.** With FIFOs, this is what bash's process substitution
  `<(...)` wants -- and it is the one thing keeping bash's own `func`
  case from passing.
- **`ls /dev` says "no such directory".** /dev is synthetic -- a name
  lookup, not a directory -- so a person cannot see what devices
  exist. A listable /dev is a VFS change, and it is the kind of thing
  somebody types on the first day.
- **No `diff`.** sbase has none. GNU diffutils (diff, cmp, diff3,
  sdiff) is the obvious port; bash's and sed's own test suites use it.
- **grep has no `-P`** (no PCRE).
- **The ported tools' own suites are shell scripts over a POSIX
  shell.** bash is on the machine now, so sed's, grep's and sbase's can
  be run there rather than approximated by a harness on the host -- and
  awk's `space` and `system-status` cases, skipped for the same
  reason, with them.

### 36. Permission enforcement

The first half is done: ext2 records a uid, a gid and a mode on every
file, a file a user creates belongs to that user, and `ls -l` prints
what is there. Storing them correctly was the prerequisite, and doing
it first is what makes enforcement possible later without rewriting
every file on the disk.

What is left is the part that **checks**: no open, no unlink, no
rename and no directory search consults a mode bit. It touches every
system call that takes a path, and it needs a decision about what root
means here.

`kernel/usertest.sh` ends with a check that reads root's file as an
ordinary user and PASSES, on purpose, so that nothing in the suite can
be read as evidence of a protection that does not exist.

Two smaller pieces belong with it: **`chmod` and `chown` are no-ops**
that answer 0 for root and EPERM otherwise, and the **set-user-id bit**
is stored and not honoured.

### 48. A Lisp: CLISP or ECL

**The dependencies are all built** -- libiconv, gettext's runtime,
readline, and GMP, which CLISP uses for bignums and which gcc needed
anyway. Nothing is missing on that side.

**The bootstrap is solved.** CLISP's build compiles a C program,
`lisp.run`, and then **runs it** to compile CLISP's own Lisp sources
into the memory image the finished system needs -- so a cross build has
to execute a target binary partway through. It can: `libc/crt0-qemu.s`
lets a statically linked Sage040 program run on the workstation under
`qemu-m68k` user-mode emulation, with no Sage040 and no kernel of ours
involved (`libc/test/qemutest.sh`, 8 checks), in seconds rather than a
minute of booting.

That route is worth more than CLISP by itself: it is the **strongest
evidence available that the ABI claim is true**, because every other
check of it in this tree is made by something in this tree, and
`qemu-m68k` is somebody else's implementation of Linux/m68k written
with no knowledge of this project.

**The problem is CLISP itself.** Its last release is **2.49, dated
2010**, and it does not build with a current gcc even on an ordinary
Linux machine. It is a porting project of its own rather than one more
port.

**ECL is the alternative and is probably the better answer.** It is
actively maintained, and it compiles Lisp to C -- which this machine
can now compile, natively, with its own gcc.

The choice is yours, and it is a real one: CLISP is the Lisp that was
asked for, and ECL is the Lisp that will build.

### 50. libatomic

CPython does not need it. The four 64-bit operations it wanted are
implemented in the C library (`atomic64.c` in the m68k backend) over a
table of locks, which is exactly what libatomic does on a processor
with no 64-bit atomic instruction -- and the 68040 has none.

What the real one would buy is a `-latomic` that EXISTS, for configure
scripts that test for it by linking, and the 16-byte operations
nothing here has asked for. Small, and worth doing when a port asks
for it by name.

### 23. Regression tests

Ongoing, never finished. The two habits that are the point of them are
written up in `os.md` under "Testing": check against something
independent, and give every fix a negative control.

---

## What is done, and where it is described

Tasks 1-22 built the system itself -- the address space, memory,
signals, pipes, subprocesses, sockets, the VT102 console, the C
library, long file names, `fsck`, shared libraries, paging and swap,
interrupt-driven I/O -- and are in [`os.md`](os.md) and
[`design.md`](design.md).

| # | | described in |
|---|---|---|
| 24-29 | awk, sed, grep, bash, the sbase utilities | `ports/*/` |
| 30 | pseudo-terminals, `PATH_MAX` | `os.md`; the rest of the task is open, above |
| 31 | CPython 3.14.7 | `ports/python/README.md` |
| 31a | threads: `clone`, futexes, pthreads | `os.md` "Threads", `libc/README.md` |
| 32 | `PATH` | `os.md` "The shell" |
| 33-35 | users, `/etc/passwd`, home directories | `os.md` "Users" |
| 37 | cron | `os.md` "Doing something later" |
| 38 | time zones | `os.md` "Time zones" |
| 39, 40 | terminfo and curses | `os.md` "Programs", `ports/ncurses/build.sh` |
| 41, 42 | ssh, scp, rsync | `os.md` "Logging in over it" |
| 43 | `/var/log`, and the kernel's log | `os.md` "The log" |
| 44 | less | `ports/less/build.sh` |
| 45, 46 | libiconv, gettext's runtime | `ports/*/build.sh` |
| 47 | readline | `ports/python/README.md` |
| 48a | the libraries CPython is built against | `ports/python/README.md` |
| 49 | a native toolchain | `toolchain.md` |
| 51 | `df` and `du` | `os.md` "Programs" and "The shell" |
| 52 | ext2, in place of FAT16 | `design.md` section 8, `os.md` "ext2" |
| | `cacheflush(2)` | `programmer-guide.md` |
| | the gcc integer-type fix | `ports/gcc/patches/02`, `toolchain.md` |
