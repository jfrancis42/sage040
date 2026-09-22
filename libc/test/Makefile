# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Programs that test the C library. They install into the root, like
# apps/, and are built against picolibc (libc.mk), not lib/ulib.

TOPDIR := ../..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := libctest

include $(TOPDIR)/libc/libc.mk

# --- shared libraries (kernel/sotest.sh) -----------------------------
#
# libsot.so twice, differing only in what sot_value() returns, and a
# program linked against it and libc.so. The .so files are built -fPIC
# with -Wl,-shared -- this bare-metal gcc does not pass a bare -shared
# on to the linker, and quietly makes an executable -- and link against
# libc.so themselves: one library that needs another. sotest is linked with -rpath-link so the linker can see
# libsot's own needs, and finds libsot.so at run time in /lib.

SO_CFLAGS := $(CFLAGS) -fPIC
SO_LDFLAGS := -mcpu=68040 -nostdlib -Wl,-shared -Wl,--hash-style=sysv \
              -Wl,-z,now -Wl,--build-id=none -L$(SAGE_LIBC)/lib

all: libsot.so libsot2.so libsot3.so sotest

libsot.so: libsot.c $(SAGE_LIBC)/lib/libc.so
	$(CC) $(SO_CFLAGS) -DSOT_VALUE=1 $(SO_LDFLAGS) -Wl,-soname,libsot.so $< -lc -o $@
libsot2.so: libsot.c $(SAGE_LIBC)/lib/libc.so
	$(CC) $(SO_CFLAGS) -DSOT_VALUE=2 $(SO_LDFLAGS) -Wl,-soname,libsot.so $< -lc -o $@
libsot3.so: libsot.c $(SAGE_LIBC)/lib/libc.so
	$(CC) $(SO_CFLAGS) -DSOT_VALUE=3 -DSOT_NO_BUMP $(SO_LDFLAGS) -Wl,-soname,libsot.so $< -lc -o $@

sotest: sotest.c libsot.so $(DYN_DEPS)
	$(CC) $(CFLAGS) $(DYN_LDFLAGS) $(LIBC_DIR)/crt0-dyn.s $< -L. -lsot $(DYN_LDLIBS) -o $@
	@$(SIZE) $@

clean: clean-so
clean-so:
	rm -f libsot.so libsot2.so libsot3.so sotest libctest.dyn
