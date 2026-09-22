#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""
synopts.py - the TCP options on the guest's SYNs, from a pcap.

  synopts.py CAPTURE.pcap GUEST_MAC

Prints one line per SYN the guest sent:

  syn mss=1460 ws=2 sackok ts=VAL,ECR

decoded here, on the host, straight from RFC 793 / 7323 / 2018 -- so an
option this stack both writes and reads wrongly, which a test over
loopback cannot see, shows up as a value that is wrong or missing.
Anything it cannot parse prints "bad ..." and exits 1.
"""
import struct
import sys


def frames(path):
    with open(path, "rb") as f:
        data = f.read()
    magic = struct.unpack("<I", data[:4])[0]
    end = "<" if magic in (0xa1b2c3d4, 0xa1b23c4d) else ">"
    off = 24
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack(end + "IIII", data[off:off + 16])
        yield data[off + 16:off + 16 + incl]
        off += 16 + incl


def options(opts):
    out, i = [], 0
    while i < len(opts):
        kind = opts[i]
        if kind == 0:
            break
        if kind == 1:
            i += 1
            continue
        if i + 1 >= len(opts) or opts[i + 1] < 2 or i + opts[i + 1] > len(opts):
            return None
        ln = opts[i + 1]
        body = opts[i + 2:i + ln]
        if kind == 2 and ln == 4:
            out.append("mss=%d" % struct.unpack(">H", body)[0])
        elif kind == 3 and ln == 3:
            out.append("ws=%d" % body[0])
        elif kind == 4 and ln == 2:
            out.append("sackok")
        elif kind == 8 and ln == 10:
            out.append("ts=%d,%d" % struct.unpack(">II", body))
        else:
            out.append("kind%d/len%d" % (kind, ln))
        i += ln
    return out


def main():
    mac = bytes.fromhex(sys.argv[2].replace(":", ""))
    bad = 0
    for fr in frames(sys.argv[1]):
        if len(fr) < 14 + 20 or fr[6:12] != mac or fr[12:14] != b"\x08\x00":
            continue
        ip = fr[14:]
        ihl = (ip[0] & 15) * 4
        if ip[9] != 6:
            continue
        tcp = ip[ihl:]
        doff = (tcp[12] >> 4) * 4
        flags = tcp[13]
        if not (flags & 0x02):
            continue
        o = options(tcp[20:doff])
        if o is None:
            print("bad options %s" % tcp[20:doff].hex())
            bad = 1
            continue
        print(("synack " if flags & 0x10 else "syn ") + " ".join(o))
    sys.exit(bad)


if __name__ == "__main__":
    main()
