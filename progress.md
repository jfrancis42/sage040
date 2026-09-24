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

**Two of bash's own tests fail: `type` and `varenv`.** Found on
2026-09-24, not yet diagnosed. They are listed in `BASH_KNOWN` in
`kernel/bashtest.sh` so that `make test` can reach the twenty suites
after it, and each prints its reason every run.

- `type` -- a function body comes back differently from what bash's
  own `type.right` has. The expected output contains a literal control
  character (`^A`, 0x01) inside the function, which makes the terminal
  or the shell's own quoting the first thing to suspect.
- `varenv` -- three `expect ...` lines are missing from the output
  entirely, so something is not being printed rather than printed
  wrongly.

**They had never run.** `export BASH_TESTS='...'` is 74 bytes with the
name, `ENV_ENTRY` in `shell.c` was 64, and `env_set` truncated in
silence and returned success -- so the last two of the eleven names
fell off the end. The check that counts how many ran could not fail
either: its message contained `$(echo $BASH_TESTS | wc -w)`, and the
shell expands arguments left to right, so `wc` set `$?` to 0 before the
`$?` that was meant to carry the result of the comparison. It printed
"[ OK ] every test asked for ran (9 of 11)" for as long as it existed.
Both are fixed; that is how these two came to light.

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
`linktest` · `logintest` ·
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
| **Rewriting published history, or not** | Three files named the private working-notes file, and `e568c44` took the name out of all three -- so no tracked file mentions it now. The references are still in HISTORY, in the two commits that introduced them: `68e3272` (`qemu-patch/build.sh` and the crypt backend) and `afa36e7` (`progress.md`). Both are pushed. Taking them out means rewriting those commits and force-pushing, which breaks every existing clone and every open checkout; leaving them means the name stays in a public repository's log. Nothing depends on the answer -- the working tree is clean either way. `git log -S <the name> origin/main` lists the commits, which is the one-line check that found this. |
| **`type` and `varenv`: diagnose now, or leave them listed?** | Two of bash's own tests fail, for reasons nobody knows yet -- see "Broken, or not working" above for what is known. They are in `BASH_KNOWN` so `make test` completes, and each prints `NOT DIAGNOSED` every run, so they cannot be forgotten. The question is only whether they are worth an afternoon before the other open work. What makes them interesting rather than routine: they had never run once, so whatever is wrong has been wrong for as long as bash has been on this machine. |

### Open, and nothing is blocking them

- **`ssh machine` still cannot get an interactive session. One cause
  fixed, a second now visible.**

  `ssh machine command` works -- `sshtest.sh` runs ten of them, plus
  scp and rsync -- and any session that asks for a terminal is refused
  by Dropbear before the shell starts.

  **Fixed:** Dropbear called `ttyname()` on the pty it had just opened,
  picolibc answered `ttyname()` by reading `/proc/self/fd/N`, and there
  is no /proc here. The kernel knows the answer -- it is where `who`
  gets "pts/0" -- so `TIOCGDEVNAME` asks a descriptor for the name of
  the character device it is open on, answered in `fd_ioctl` from the
  device registry so that every character device has a name for free.
  `libc/patches/37` rewrites `ttyname_r` on it. `tty(1)` wants the same
  call.

  **Next:** `chown(/dev/pts/0, 1000, 1000) failed: No such file or
  directory`. Dropbear hands the pty to the user who logged in, and the
  chown fails -- on a path the kernel resolves perfectly well for open,
  which points at `chown` not resolving `/dev/` names rather than at
  anything to do with ptys. `chmod` on the same path is the next line
  and will need the same. Whether Dropbear should be doing this at all
  is a separate question: the pty is already the right owner, since it
  was created by a process that is about to become that user.

  Found by `whotest.sh`, which logs in over ssh to check that `who`
  sees a remote user. It still cannot, because nobody can -- and the
  suite says so as a NOTE naming the current symptom, so the day the
  symptom changes it goes back to being a failure.

- **Nothing on this machine ever calls `setsid()`, so sessions are not
  what they claim to be.**

  The call is implemented (`syslinux.c`) and `struct task` carries a
  `sid`, but no program uses it, so every task on the console belongs
  to the session of the shell that booted -- `idle`, `netd`, `klogd`
  and whoever is logged in, all one session. Ssh is the same: Dropbear's
  session handling was never ported, so a connection's shell inherits
  whatever session the daemon had.

  This was found by writing `who`, whose natural rule -- a login is a
  session leader with a terminal -- reported the kernel's own `idle`
  and `netd` tasks as two logged-in roots and missed the person at the
  keyboard entirely. `who` uses argv[0] instead and says why at length;
  it is right either way, so nothing is blocked on this.

  What it would take: `login` cannot simply call `setsid()`, because
  the console shell spawns it as a job of its own and POSIX makes the
  call fail for a process-group leader (the check is in `syslinux.c`,
  and it is correct). It would have to fork first and let the child
  start the session, which is what a getty does. Dropbear would need
  the same in its own child.

  What it would buy, beyond a tidier `who`: a controlling terminal that
  means something, SIGHUP to a session when its terminal goes away --
  which is what should happen when an ssh connection drops and at
  present does not -- and `ps` being able to group a machine's tasks by
  who is running them.

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
- **MMU: turn the CACHES on. DONE.** `cache_enable()` writes
  CACR = 0x80008000 -- data cache bit 31, instruction cache bit 15 --
  at the end of the boot sweep in `kernel/vm.c`, and the banner says
  "caches : data on, instruction on, tables non-cachable".
  The sweep is the part that is not obvious: it walks root -> pointer
  -> page tables and marks every table page non-cachable
  (`vm_table_nocache`, `kset_cachemode`), because **the 68040's table
  walker does not snoop the data cache**. A descriptor written by the
  kernel and still sitting dirty in the data cache is a descriptor the
  hardware would not see. Everything else was already right:
  descriptors carry CM copyback for RAM and non-cachable for I/O, and
  the transparent-translation registers cover the I/O and framebuffer
  windows.
  **NONE OF IT CAN BE VERIFIED HERE.** QEMU models no cache: it
  ignores the CM bits entirely and decodes `cinv` and `cpush` as
  privileged no-ops, so a correct cache setup and a broken one are
  indistinguishable and there is no speedup to measure either. It is
  for real hardware, and it is correct by inspection rather than by
  test -- which is why that is said at every place it is switched on.
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
  somewhere unrelated, which is close to undebuggable. And the benefit cannot be measured here: it is ATC
  refill traffic on hardware that does not exist yet.


- **`sudo` passes the caller's whole environment through, and searches
  the caller's `PATH`.** Real sudo resets both (`env_reset`,
  `secure_path`). Here `execvp` means `sudo make` runs whatever `make`
  the caller's PATH finds first, as root.
  **This is not an escalation as sudoers stands**, and the reason is
  worth writing down rather than rediscovering: the only rule the
  parser accepts is `ALL`, so anybody sudo will run anything for could
  equally have typed the full path. It becomes a hole the moment
  sudoers learns to restrict WHICH commands, because then the command
  name is a decision and PATH decides what it means. Whoever adds
  command lists has to do `secure_path` in the same change.
- **The POSIX gaps that are left** (30): FIFOs, `/dev/fd`, a listable
  `/dev`, and `diff`. Detailed below.
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
- **Object files built on the machine are not byte-reproducible.** The
  native assembler leaves uninitialised bytes in section padding where
  the cross one leaves zeroes. Every section a tool reads is identical.
- **`xz` at its default preset WORKS now, and that is a change.** `-6`
  wants about 94 MB for its dictionary; the machine had 64 when this
  was written and has 256, so it is affordable. `pylibtest` had a check
  asserting the default FAILED -- correct at 64 MB, wrong since -- and
  it is what caught this. The suite now picks the preset to refuse from
  RAM_MB (`-9` wants ~674 MB) and separately asserts the default
  succeeds, so neither half goes stale when the machine's size changes
  again. What is being tested is that an unaffordable preset is refused
  cleanly, not which preset that is.
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
| 36 | logins and passwords, `/etc/shadow`, `crypt(3)`, `login`/`su`/`sudo`/`passwd`/`useradd`/`userdel` | `os.md` "Logging in" |
| 36 | permission enforcement, set-user-id on exec, real `chmod`/`chown` | `os.md` "What is enforced" |
| | ssh by password, against the same shadow file | `os.md` "Logging in over it", `ports/dropbear/localoptions.h` |
| | hard links and symlinks, fast and slow, `lstat` | `os.md` "Links" |
| | the 68040's caches, and non-cachable page tables | `os.md` "Memory", `design.md` |
| | `cacheflush(2)` | `programmer-guide.md` |
| | the gcc integer-type fix | `ports/gcc/patches/02`, `toolchain.md` |
