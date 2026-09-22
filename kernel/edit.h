/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * edit.h - reading a line, the way bash reads one.
 *
 * This is a readline, not a kernel facility, and the distinction is the
 * point. The kernel's terminal knows how to assemble a line with erase
 * and kill and nothing more; everything here -- moving the cursor,
 * history, searching it -- is done by turning canonical mode off and
 * doing the work above the system call boundary, which is exactly where
 * bash does it and for exactly the same reason. Putting ctrl-R in a
 * kernel would be putting a shell's memory inside the machine.
 *
 * So this file reaches the system only through syscall.h. It compiles
 * unchanged the day the shell stops being linked into the kernel.
 */
#ifndef EDIT_H
#define EDIT_H

#include "types.h"

/*
 * Read one line, editing it. Returns its length, or:
 *
 *   -EINTR   ctrl-C: the line was abandoned, print a new prompt
 *   -EIO     end of input on an empty line
 *
 * The result is NUL terminated and carries no newline.
 */
int edit_readline(const char *prompt, char *buf, int max);

/* Add a line to the history, unless it repeats the last one or is
 * blank -- the two cases nobody ever wants to scroll back through. */
void edit_history_add(const char *line);

/* Walk it, newest last. Returns 0 past the end. */
const char *edit_history_nth(int index);
int  edit_history_count(void);

#endif /* EDIT_H */
