#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""
netservers.py - a DNS server and an SNTP server, for dnstest.sh.

Both on unprivileged ports on the host, reached by the guest through
QEMU's user-mode network at 10.0.2.2, so the tests need no network
beyond the machine they run on -- and give answers that are known in
advance, which a real resolver's would not be.

  netservers.py DNS_PORT NTP_PORT NTP_TIME

DNS answers A queries from ZONE below: an address, a CNAME followed by
the address it leads to, NXDOMAIN for anything it does not know, and
nothing at all for "silent.sage.test", so a client's timeout can be
seen. SNTP answers every request with NTP_TIME (Unix seconds), stratum 2.
"""
import select
import socket
import struct
import sys

ZONE = {
    "foo.sage.test": ("A", "10.1.2.3"),
    "gw.sage.test": ("A", "10.0.2.2"),
    "ntp.sage.test": ("A", "10.0.2.2"),
    "alias.sage.test": ("CNAME", "foo.sage.test"),
    "chain.sage.test": ("CNAME", "alias.sage.test"),
    "UPPER.sage.test": ("A", "10.9.9.9"),
    "spoof.sage.test": ("A", "10.7.7.7"),
}
# Answered twice: first with the wrong ID and 6.6.6.6, as a forger racing
# the real server would, then properly. A resolver that believes the
# first datagram to arrive gets the wrong address.
SPOOF = "spoof.sage.test"
SILENT = "silent.sage.test"
NTP_UNIX = 2208988800


def qname(pkt, off):
    labels = []
    while pkt[off]:
        n = pkt[off]
        labels.append(pkt[off + 1:off + 1 + n].decode("ascii", "replace"))
        off += 1 + n
    return ".".join(labels), off + 1


def encode(name):
    out = b""
    for label in name.split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\0"


def lookup(name):
    for k, v in ZONE.items():
        if k.lower() == name.lower():
            return v
    return None


def dns_answer(pkt):
    (qid, flags, qd) = struct.unpack(">HHH", pkt[:6])
    name, off = qname(pkt, 12)
    question = pkt[12:off + 4]
    if name.lower() == SILENT:
        return None
    answers = []
    cur = name
    for _ in range(8):
        rec = lookup(cur)
        if rec is None:
            break
        kind, value = rec
        if kind == "CNAME":
            answers.append(encode(cur) + struct.pack(">HHIH", 5, 1, 60,
                           len(encode(value))) + encode(value))
            cur = value
        else:
            addr = socket.inet_aton(value)
            answers.append(encode(cur) + struct.pack(">HHIH", 1, 1, 60, 4) + addr)
            break
    rcode = 0 if answers else 3
    hdr = struct.pack(">HHHHHH", qid, 0x8180 | rcode, 1, len(answers), 0, 0)
    return hdr + question + b"".join(answers)


def ntp_answer(pkt, t):
    if len(pkt) < 48:
        return None
    secs = (t + NTP_UNIX) & 0xffffffff       # era 1 after 2036
    stamp = struct.pack(">II", secs, 0)
    return (bytes([(0 << 6) | (4 << 3) | 4, 2, 6, 0xec]) +
            b"\0" * 8 +               # root delay, dispersion
            b"SAGE" +                 # reference id
            stamp +                   # reference
            pkt[40:48] +              # origin: the client's transmit
            stamp + stamp)            # receive, transmit


def main():
    dns_port, ntp_port, ntp_time = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dns.bind(("0.0.0.0", dns_port))
    ntp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ntp.bind(("0.0.0.0", ntp_port))
    # A second NTP server, one port up, whose clock is past 2036 -- when
    # NTP's 32-bit seconds wrap and a naive client lands in 1900.
    late = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    late.bind(("0.0.0.0", ntp_port + 1))
    late_time = int(sys.argv[4]) if len(sys.argv) > 4 else ntp_time
    print("netservers: dns %d, ntp %d and %d" % (dns_port, ntp_port,
                                                 ntp_port + 1), flush=True)
    while True:
        r, _, _ = select.select([dns, ntp, late], [], [])
        for s in r:
            pkt, peer = s.recvfrom(1500)
            if s is dns:
                out = dns_answer(pkt)
                if out and qname(pkt, 12)[0].lower() == SPOOF:
                    bad = bytearray(out)
                    bad[0] ^= 0xff
                    bad[-4:] = socket.inet_aton("6.6.6.6")
                    s.sendto(bytes(bad), peer)
                print("dns query for %s from %s" % (qname(pkt, 12)[0], peer),
                      flush=True)
            else:
                out = ntp_answer(pkt, ntp_time if s is ntp else late_time)
                print("ntp query from %s" % (peer,), flush=True)
            if out is not None:
                s.sendto(out, peer)


if __name__ == "__main__":
    main()
