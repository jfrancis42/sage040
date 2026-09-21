# Progress

Working log for the port-enablement work: making Sage040 able to build
and run software written by other people, with a real editor as the
proof. `design.md` §11 is the reasoned list; this file is the order, and
the running state as it actually is.

**Goal: completeness, usability and ease of porting.** Not speed of
implementation, and not economy of RAM or disk — both can be increased
and will be where they get in the way.

**Status: nothing started. 0 of 20 tasks done.**

Each entry is filled in *when the work is finished and tested*, not
before. If a task is listed as done, the tests for it pass.

---

## Order, and why

Sorted so each task makes the ones after it easier. Four dependencies
drive almost all of it:

1. **Address space before memory.** `brk` is meaningless inside 2 MB.
2. **Memory before a libc.** `malloc` needs pages to hand out.
3. **A libc before any port.** Nothing real builds without one.
4. **Signals and `select` before an editor.** `SIGWINCH`, and waiting on
   input with a timeout, are what an interactive program is built on.

| # | Task | Why here | State |
|---|------|----------|-------|
| 0 | Groundwork: bigger RAM and disk, per-task cwd, `fstat`/`access`/`dup` | small things everything else trips over | todo |
| 1 | Grow the user address space | nothing else fits until this | todo |
| 2 | `brk`/`sbrk` | the smaller half of memory; enough for `malloc` | todo |
| 3 | `mmap`/`munmap`/`mprotect` | what a runtime and a libc expect | todo |
| 4 | `malloc` in `lib/` | so tasks 5–12 have something to test against | todo |
| 5 | Signals: `sigaction`, handlers, `sigreturn` | the largest kernel item; editors need it | todo |
| 6 | `select`/`poll` | the other half of an event loop | todo |
| 7 | Interval timers | depends on 5 | todo |
| 8 | Pipes, `dup2`, shell redirection | visible win; needed by 9 | todo |
| 9 | Subprocesses: `fork`/`execve`/`waitpid` | `:!` and `:make` in an editor | todo |
| 10 | The rest of the socket API, and the signatures | before a libc is written against the old ones | todo |
| 11 | VT100 emulation in `fbcon.c` | a full-screen program needs cursor addressing | todo |
| 12 | `TIOCGWINSZ` and `SIGWINCH` | depends on 5 and 11 | todo |
| 13 | A C library (picolibc or newlib) | the gate everything real passes through | todo |
| 14 | VFAT long file names | 8.3 decides what can be shipped | todo |
| 15 | `fsck` | the machine cannot check its own disk | todo |
| 16 | Build and run uEmacs | the cheapest real editor | todo |
| 17 | Build and run vi | the other one | todo |
| 18 | A resolver | independent; do it when convenient | todo |
| 19 | Regression tests throughout | every task ships with its tests | ongoing |

Tasks 16 and 17 are the point of the exercise. Everything before them is
what they need.

---

## Log

*Empty. Entries are added as tasks complete, with the decisions made
and anything surprising found along the way.*

---

## Decisions worth knowing about

*Added as they are made.*

## Still open, deliberately

- **Demand paging and swap.** In `design.md` §11 and not in this list:
  nothing being ported needs it, and eager mappings are simpler to trust.
- **Shared libraries.** Downstream of the libc; every program is
  statically linked and that is fine at this size.
- **The remaining TCP options** (window scaling, SACK, timestamps).
