# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# Logging in, and changing who you are.
#
# These are in a directory of their own, and built against PICOLIBC
# rather than lib/ulib like system/, for one reason: they need
# crypt(3), and crypt lives in the C library. getpwnam and the rest of
# the account machinery are there too. They are bigger than a system/
# program as a result -- about 100 KB against 5 -- and they run once,
# at a login, so that costs nothing that matters.
#
# THREE OF THEM ARE SET-USER-ID ROOT, and that is the point of them:
#
#   passwd  writes /etc/shadow, which is 0600 root
#   su      becomes another user
#   sudo    runs one command as another user
#
# A set-user-id program is the only way an ordinary user does a
# privileged thing, and it is also the only kind of program where a
# mistake hands over the machine. Each drops privilege as soon as it
# can and checks that the drop WORKED.

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := login su sudo passwd useradd userdel

include $(TOPDIR)/libc/libc.mk

# Every one of them is login.c-and-pwdb.c, not just login.c, so the
# single-source rule in libc.mk does not fit.
SHARED := pwdb.c pwdb.h

$(PROGS): %: %.c $(SHARED) $(LIBC_DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(CRT0) $< pwdb.c $(LDLIBS) -o $@
	@if $(READELF) -l $@ | grep -q INTERP; then \
	    echo "auth: $@ came out dynamic from a static link" >&2; \
	    rm -f $@; exit 1; fi
	@$(SIZE) $@

# The modes matter more here than anywhere else on the disk, so they
# are written out one program at a time rather than looped over:
# 4755 on the wrong file is a root shell for anybody.
install: $(PROGS) $(DISK)
	@$(FSIMG) put -m 755 login   /bin/login
	@$(FSIMG) put -m 755 useradd /bin/useradd
	@$(FSIMG) put -m 755 userdel /bin/userdel
	@$(FSIMG) put -m 4755 su     /bin/su
	@$(FSIMG) put -m 4755 sudo   /bin/sudo
	@$(FSIMG) put -m 4755 passwd /bin/passwd
	@if ! $(FSIMG) exists /etc/sudoers; then \
	    $(FSIMG) put -m 440 $(TOPDIR)/system/sudoers /etc/sudoers && \
	    echo "installed /etc/sudoers"; \
	fi
	@echo "login, su, sudo, passwd, useradd, userdel -> /bin"
