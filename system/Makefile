# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# The system's own programs, which live in /bin.
#
# These are part of the machine rather than things somebody chose to run
# on it: configuring the interface, asking whether a host is reachable,
# showing what the network layer is holding, stopping the machine. A
# fresh disk is expected to have them and PATH looks here first.
#
# They are ordinary unprivileged programs all the same. ifconfig runs in
# an address space of its own and can do exactly what the system call
# interface allows -- being part of the system buys it no privilege,
# which is the property worth having.

TOPDIR := ..
include $(TOPDIR)/disk.mk

.DEFAULT_GOAL := all

PROGS := ifconfig ping netstat route arp shutdown reboot env stty resize fsck host ntpdate swapon swapoff irqs nvram klogd dmesg df id who w uptime sh
INSTALL_DIR := /bin

include $(TOPDIR)/lib/program.mk

# /etc/rc, and the /var tree klogd logs into.
#
# THE RULE, because both halves of it matter: an /etc/rc that still says
# "SuckOS default" in its first line is one nobody has edited, so it is
# replaced and keeps up with the system. One that does not is a
# configuration somebody wrote, and is left alone. A disk with no
# /etc/rc gets the default.
#
# The alternative -- never overwrite -- is what left a machine booting a
# one-line placeholder from an old test long after the default had
# grown a log daemon and a time zone. `make etc` replaces it either way.
install: install-etc
install-etc: $(DISK)
	@for d in /etc /root /home /home/jfrancis /var /var/log /var/run; do \
	    $(FSIMG) mkdir $$d; \
	 done
	@# /etc/passwd and /etc/group: installed if missing, left alone if
	@# they are there. Unlike /etc/rc there is no "unedited default" to
	@# recognise -- a passwd file is a list of people, and replacing one
	@# because it looked like the shipped version would delete a user.
	@if ! $(FSIMG) exists /etc/passwd; then \
	    $(FSIMG) put passwd /etc/passwd && echo "installed /etc/passwd"; \
	fi
	@if ! $(FSIMG) exists /etc/group; then \
	    $(FSIMG) put group /etc/group && echo "installed /etc/group"; \
	fi
	@# /etc/shadow, MODE 0600. The mode is the whole point of the file
	@# existing separately, and it is only worth anything because the
	@# kernel enforces one now. Installed if missing and never
	@# replaced: overwriting it would reset every password on the
	@# machine back to the shipped default.
	@# /etc/profile and a ~/.profile for each account that has a home
	@# on a fresh disk. Installed only if missing: these are files
	@# somebody edits, and replacing an edited one would throw away
	@# what they wrote.
	@if ! $(FSIMG) exists /etc/profile; then \
	    $(FSIMG) put profile /etc/profile && echo "installed /etc/profile"; \
	fi
	@for h in /root /home/jfrancis; do \
	    if ! $(FSIMG) exists $$h/.profile; then \
	        $(FSIMG) put dot-profile $$h/.profile && \
	        echo "installed $$h/.profile"; \
	    fi; \
	 done
	@# jfrancis logs into bash, and a bash LOGIN shell reads ~/.bash_profile
	@# (or ~/.profile), never ~/.bashrc -- so without a .bash_profile that
	@# sources it, a personal .bashrc silently never runs at login. Ship
	@# both: the .bash_profile that sources .bashrc, and a starter .bashrc.
	@if ! $(FSIMG) exists /home/jfrancis/.bash_profile; then \
	    $(FSIMG) put dot-bash_profile /home/jfrancis/.bash_profile && \
	    echo "installed /home/jfrancis/.bash_profile"; \
	fi
	@if ! $(FSIMG) exists /home/jfrancis/.bashrc; then \
	    $(FSIMG) put dot-bashrc /home/jfrancis/.bashrc && \
	    echo "installed /home/jfrancis/.bashrc"; \
	fi
	@# THE HOME DIRECTORY MUST BE OWNED BY ITS PERSON, or they cannot
	@# create a file in it -- mkdir above made it root's, and the symptom
	@# is "I have to sudo to write to my own home". jfrancis is uid/gid
	@# 1000 in the shipped passwd; chown the directory and everything the
	@# fresh install put in it. Idempotent, so re-running install is safe.
	@$(FSIMG) chown /home/jfrancis 1000:1000
	@for f in .profile .bash_profile .bashrc; do \
	    if $(FSIMG) exists /home/jfrancis/$$f; then \
	        $(FSIMG) chown /home/jfrancis/$$f 1000:1000; \
	    fi; \
	 done
	@if ! $(FSIMG) exists /etc/shadow; then \
	    $(FSIMG) put -m 600 shadow /etc/shadow && \
	    echo "installed /etc/shadow (root's password is 'root')"; \
	fi
	@if ! $(FSIMG) exists /etc/rc; then \
	    $(FSIMG) put rc /etc/rc && echo "installed /etc/rc"; \
	elif $(FSIMG) cat /etc/rc 2>/dev/null | head -1 | \
	     grep -q "SuckOS default"; then \
	    $(FSIMG) put rc /etc/rc && \
	    echo "/etc/rc was the unedited default: brought up to date"; \
	else \
	    echo "/etc/rc has been edited, and is yours: not touched"; \
	fi

# Replace /etc/rc whatever it says.
etc: $(DISK)
	@$(FSIMG) mkdir /etc
	$(FSIMG) put rc /etc/rc
	@echo "/etc/rc replaced with the default"


# sh is the kernel's own shell, built as a program: the shell and line
# editor sources, the gate's calling side, and the pure helpers they use
# -- and NOT ulib, whose string functions would collide with the
# kernel's own string.c that the shell is written against.
K := $(TOPDIR)/kernel
SH_SRCS := sh.c $(K)/shell.c $(K)/edit.c $(K)/sysuser.c $(K)/string.c \
           $(K)/errno.c $(K)/time.c

sh: $(SH_SRCS) $(LIB)/crt0.s $(LIB)/user.ld $(K)/syscall.h $(K)/uapi.h \
    $(TOPDIR)/types.h $(K)/edit.h
	$(CC) $(CFLAGS) $(LDFLAGS) \
	    -x assembler-with-cpp $(LIB)/crt0.s \
	    -x c $(SH_SRCS) -o $@
	@$(SIZE) $@
