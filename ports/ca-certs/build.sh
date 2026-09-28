#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - the certificate authorities the machine trusts.
#
# OpenSSL was built with --openssldir=/etc/ssl, so the file it reads
# when a program asks for "the default trust store" is /etc/ssl/cert.pem.
# Until this port, nothing put anything there: the library was complete
# and every verified TLS connection had nothing to verify against.
# Nothing here is compiled; this only fetches the
# bundle and proves it is the one pinned below.
#
# THE BUNDLE is Mozilla's root store as curl's project extracts it
# (https://curl.se/docs/caextract.html): PEM, one file, what curl,
# Python and OpenSSL all take as they stand. It is pinned by date and
# hash like every other port's source, so the machine's trust changes
# when somebody edits this file and not when curl.se publishes. To
# update: pick the newest dated file on that page, and take its hash
# from the .sha256 file published beside it.
#
# /etc/ssl/certs, the hashed-directory form, is deliberately absent. It
# is a second copy of the same trust that would have to be kept in step
# with this one, and nothing on the machine needs it.

set -eu

cd "$(dirname "$0")"

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}

DATE=2026-09-25
SHA256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505
URL=https://curl.se/ca/cacert-$DATE.pem
OUT=$SRCDIR/ca-certs
PEM=$OUT/cacert-$DATE.pem

mkdir -p "$OUT"
if [ ! -f "$PEM" ]; then
    curl -L --fail -o "$PEM.part" "$URL"
    mv "$PEM.part" "$PEM"
fi
echo "$SHA256  $PEM" | sha256sum -c --quiet - || {
    echo "ca-certs: $PEM is not the pinned bundle -- delete it to refetch" >&2
    exit 1
}
ln -sf "cacert-$DATE.pem" "$OUT/cert.pem"

echo "ca-certs $DATE -> $OUT/cert.pem ($(grep -c 'BEGIN CERTIFICATE' "$PEM") CAs)"
