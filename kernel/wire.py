#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
"""
wire.py - the other end of the guest's Ethernet cable, for lotest.sh.

QEMU's `-nic socket,udp=HOST:TX,localaddr=HOST:RX` carries each Ethernet
frame as one UDP datagram: what the guest sends arrives at TX, and what
is sent to RX arrives at the guest as if from the wire. That makes it
possible to put frames on the wire that no well-behaved host would send
-- which is the point.

  wire.py capture TXPORT FILE
      Write every frame the guest sends to FILE, one hex line each,
      until killed.

  wire.py send RXPORT DSTMAC SRCIP DSTIP DPORT PAYLOAD
      Put one UDP datagram on the wire, in a frame addressed to DSTMAC.

  wire.py summary FILE
      How many frames the guest sent, and how many of them carried a
      127/8 address in either IP field.
"""
import socket
import struct
import sys


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def frame(dstmac, srcip, dstip, dport, payload):
    src = socket.inet_aton(srcip)
    dst = socket.inet_aton(dstip)
    udp = struct.pack("!HHHH", 40000, dport, 8 + len(payload), 0) + payload
    pseudo = src + dst + struct.pack("!BBH", 0, 17, len(udp))
    udp = udp[:6] + struct.pack("!H", csum(pseudo + udp) or 0xFFFF) + udp[8:]
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0,
                     src, dst)
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    mac = bytes(int(x, 16) for x in dstmac.split(":"))
    return mac + bytes.fromhex("525400000001") + b"\x08\x00" + ip + udp


def main():
    cmd = sys.argv[1]
    if cmd == "capture":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", int(sys.argv[2])))
        with open(sys.argv[3], "w") as out:
            while True:
                data = s.recv(65536)
                out.write(data.hex() + "\n")
                out.flush()
    elif cmd == "send":
        port, mac, sip, dip, dport, payload = sys.argv[2:8]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(frame(mac, sip, dip, int(dport), payload.encode()),
                 ("127.0.0.1", int(port)))
    elif cmd == "summary":
        total = martian = 0
        for line in open(sys.argv[2]):
            f = bytes.fromhex(line.strip())
            total += 1
            if len(f) >= 34 and f[12:14] == b"\x08\x00":
                if f[26] == 127 or f[30] == 127:
                    martian += 1
        print(total, martian)


if __name__ == "__main__":
    main()
