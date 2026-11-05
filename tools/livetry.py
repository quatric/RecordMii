#!/usr/bin/env python3
"""Pokes a running RecordMii's Wii Speak and reports whether the stream moves.

    livetry.py HOST

Each step sends console commands, then counts distinct 32-byte packets in
half a second of raw transfers (~490 if live, 8 if one block is replayed).
Also reads back every register it can.  Leaves the app running."""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wsdbg  # noqa: E402


def live(con):
    ok, _, blob, info, _ = con.cmd("cap 500")
    if not ok or not blob:
        return "no data"
    recs = wsdbg.parse_records(blob, info[0])
    pkts = [r[2][i * 32:(i + 1) * 32] for r in recs if r[0] >= 0 for i in range(8)]
    return "%d/%d distinct%s" % (len(set(pkts)), len(pkts), "  <-- MOVING" if len(set(pkts)) > 16 else "")


def run(con, label, cmds):
    for c in cmds:
        ok, text, _, _, st = con.cmd(c)
        print("    > %-22s %s" % (c, st.strip() or text.strip()), flush=True)
    time.sleep(0.4)
    print("%-34s %s" % (label, live(con)), flush=True)


STEPS = [
    ("baseline", []),
    ("echo canceller on", ["reg 14 1 0 r"]),
    ("echo canceller off", ["reg 14 0 0 r"]),
    ("gain 0 (0 dB)", ["reg 4 0 0 r", "reg 5 0 0 r"]),
    ("gain 1", ["reg 4 1 0 r", "reg 5 0 0 r"]),
    ("gain 2", ["reg 4 2 0 r", "reg 5 0 0 r"]),
    ("gain 3", ["reg 4 3 0 r", "reg 5 0 0 r"]),
    ("freq 8 kHz", ["reg 2 1 0 r", "reg 3 0 0 r"]),
    ("freq 11 kHz", ["reg 2 2 0 r", "reg 3 0 0 r"]),
    ("freq 16 kHz", ["reg 2 3 0 r", "reg 3 0 0 r"]),
    ("sp enable", ["sp 1"]),
    ("sp disable", ["sp 0"]),
    ("sampler off", ["reg 0 0 0 r"]),
    ("sampler on again", ["reg 0 1 0 r", "reg c 0 0 r"]),
    ("sampler on x2 + mute 1 then 0", ["reg c 1 0 r", "reg c 0 0 r"]),
]


def main():
    con = wsdbg.Console(sys.argv[1])
    print(con.cmd("status")[1])
    print("\n== register scan (reads, arg 0 0) ==")
    for reg in range(0, 0x40, 2):
        ok, _, _, _, st = con.cmd("reg %x 0 0 r" % (reg | 1))
        print("  reg %02x  %s" % (reg, st.strip()), flush=True)
    print("\n== steps ==")
    for label, cmds in STEPS:
        run(con, label, cmds)
    print("\n== stats ==")
    print(con.cmd("stats")[1])


main()
