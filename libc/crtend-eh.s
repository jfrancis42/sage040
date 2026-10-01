| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| crtend-eh.s - the end of a program's .eh_frame: a zero length word,
| which is where the unwinder stops walking it. crtend.o's __FRAME_END__
| on a stock system; see crtbegin-eh.s.

        .section .eh_frame,"a",@progbits
        .p2align 2
        .long   0

| No executable stack: without this note ld warns, and assumes one.
        .section .note.GNU-stack,"",@progbits
