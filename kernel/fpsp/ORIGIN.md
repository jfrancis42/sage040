# Motorola's M68040 Floating-Point Software Package

The MC68040 implements only part of the MC68881/MC68882 instruction
set in silicon. FSIN, FETOX, FLOGN and the rest of the transcendentals,
FINT, FMOD, FREM, FSCALE, FMOVECR and a few more (MC68040 User's
Manual, Table 9-10) raise the *unimplemented floating-point
instruction* exception instead, and denormalised, unnormalised and
packed-decimal operands raise *unsupported data type*. Motorola's FPSP
is the handler for both: it decodes the instruction from the FPU's
state frame, computes the result to the MC68881's precision, and
resumes the program. It also completes the arithmetic exceptions --
overflow, underflow, operand error, signalling NaN, BSUN -- the way
IEEE 754 and the MC68881 specify.

This directory is that package, as distributed with Linux in
`arch/m68k/fpsp040` (last changed there by commit `9faf1f1a55ee`,
2023-10-16).

## What was changed, as the licence requires it be said

- **The files are renamed from `.S` to `.s`.** This kernel's build
  sends every `.s` through the C preprocessor (`-x
  assembler-with-cpp`), as Linux's does for `.S`; the build and the
  native `kernel/build.sh` both select files by that suffix. The
  contents are byte for byte those of the Linux copy.
- **`skeleton.S` is not here.** It is the part of the package every
  host operating system replaces with its own; this kernel's is
  `kernel/fpspglue.s`, written for this kernel, which keeps Motorola's
  documented E1/E3 clean-up sequences from `skeleton.sa` and says so.
- Linux's `Makefile` is not here.

Nothing else is modified. Motorola's licence is in `README`, unaltered,
and applies to every file in this directory except this one.

## The interface to the kernel

The package defines the entry points the exception vectors point at
(`fpsp_fline`, `fpsp_bsun`, `fpsp_unfl`, `fpsp_operr`, `fpsp_ovfl`,
`fpsp_snan`, `fpsp_unsupp`) and expects the host to supply twelve
symbols: `fpsp_done` (resume the program), `mem_read` and `mem_write`
(copy up to 12 bytes to or from the program), `fpsp_fmt_error` (an FPU
state frame of a kind the package does not know), `real_trace`, and
`real_bsun`, `real_fline`, `real_inex`, `real_operr`, `real_ovfl`,
`real_snan`, `real_unfl` (the exception is real and must be reported).
`kernel/fpspglue.s` supplies all of them.
