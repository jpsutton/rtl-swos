#!/usr/bin/env python3
# Needs CAP_NET_RAW (root, or e.g. docker run --net=host --cap-add=NET_RAW).
"""Minimal LACP partner on a raw interface, for bench-testing the switch.

  lacp-partner.py IFACE SECONDS [sniff|answer] [sync|nosync]

sniff: print LACPDUs seen. answer: reply to each one (and once a second)
as an active, aggregatable partner with the short timeout, echoing the
switch's actor info so the switch sees itself matched."""
import socket, struct, sys, time, select

iface, secs = sys.argv[1], float(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else "sniff"
sync = (sys.argv[4] if len(sys.argv) > 4 else "sync") == "sync"
DST = bytes.fromhex("0180c2000002")
ME = bytes.fromhex("020000000099")

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x8809))
s.bind((iface, 0))

ST = {0x01: "act", 0x02: "fast", 0x04: "agg", 0x08: "sync", 0x10: "col", 0x20: "dist",
      0x40: "dflt", 0x80: "exp"}


def st_txt(v):
    return "|".join(n for b, n in ST.items() if v & b) or "-"


def info(b):
    sp, sysm, key, pp, port, st = struct.unpack("!H6sHHHB", b[:15])
    return f"sys={sp},{sysm.hex()} key={key} port={port} st={st_txt(st)}"


last_actor = None
end = time.time() + secs
next_tx = 0
while time.time() < end:
    r, _, _ = select.select([s], [], [], 0.2)
    if r:
        f = s.recv(2000)
        if f[:6] == DST and f[12:14] == b"\x88\x09" and f[14] == 1:
            pdu = f[14:]
            last_actor = pdu[4:19]
            print(f"{time.time() % 1000:7.2f} rx actor[{info(pdu[4:19])}] partner[{info(pdu[24:39])}]",
                  flush=True)
            if mode == "answer":
                next_tx = 0
    if mode == "answer" and last_actor and time.time() >= next_tx:
        st = 0x01 | 0x02 | 0x04 | (0x08 | 0x10 | 0x20 if sync else 0)
        actor = struct.pack("!H6sHHHB3x", 32768, ME, 7, 32768, 1, st)
        pdu = bytes([1, 1, 1, 20]) + actor + bytes([2, 20]) + last_actor + b"\0\0\0" \
            + bytes([3, 16]) + bytes(14) + bytes(52)
        s.send(DST + ME + b"\x88\x09" + pdu)
        next_tx = time.time() + 1.0
