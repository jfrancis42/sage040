#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# cryptotest.sh - the kernel's BLAKE2s and ChaCha20 (crypto.c) against
# the RFCs' test vectors, built for the host. libctest.sh runs the same
# program on the machine, which is big-endian where this host is not.

set -u
cd "$(dirname "$0")"
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
cc -O2 -Wall -Wextra -iquote . -o "$SCRATCH/cryptotest" cryptotest.c crypto.c || exit 1
"$SCRATCH/cryptotest"
r=$?
echo
if [ $r -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
