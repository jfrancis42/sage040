/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2026 Jeff Francis
 *
 * termcap.h - the termcap interface, for a machine with one terminal.
 *
 * BSD-licensed, not GPL like the rest of the tree, because it is linked
 * into programs that are neither -- uEmacs among them.
 *
 * PC, UP, BC and ospeed exist in the library for programs that assign
 * them, and are deliberately NOT declared here: some programs declare
 * their own of the same names, static, and a declaration here would
 * collide with them.
 */
#ifndef _TERMCAP_H_
#define _TERMCAP_H_

#ifdef __cplusplus
extern "C" {
#endif

int   tgetent(char *bp, const char *name);
int   tgetflag(const char *id);
int   tgetnum(const char *id);
char *tgetstr(const char *id, char **area);
char *tgoto(const char *cap, int col, int row);
int   tputs(const char *str, int affcnt, int (*putc)(int));

#ifdef __cplusplus
}
#endif

#endif /* _TERMCAP_H_ */
