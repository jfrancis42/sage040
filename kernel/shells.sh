# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# shells.sh - the shells on a suite's scratch disk, laid out as a real
# disk has them. Sourced by the suites; each has its own fsimg().
#
#     put_shells          /bin/msh, and /bin/sh -> msh
#     put_shells bash     /bin/msh, /bin/bash, and /bin/sh -> bash
#
# The rule is the install rules' own (system/Makefile, ports/bash's
# Makefile): the system shell is /bin/msh, and /bin/sh is bash wherever
# bash is, or msh on a disk without it. It is in one place because
# thirty suites used to put the system shell AT /bin/sh by hand, and the
# day /bin/sh became bash every one of them went on testing a machine
# nobody had any more -- which only shows when a suite runs make, a
# configure script or system() and meets a /bin/sh no real disk has.

put_shells() {
    fsimg put -m 755 ../system/sh /bin/msh
    if [ "${1:-}" = bash ]; then
        fsimg put -m 755 ../ports/bash/bash /bin/bash
        fsimg symlink bash /bin/sh
    else
        fsimg symlink msh /bin/sh
    fi
}
