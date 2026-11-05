#!/usr/bin/env python3
"""Checks which runtime setting writes keep the Wii Speak streaming.

    cfgprobe.py HOST

Records in silence and with a 1 kHz tone on the TV, first as started, then
after each register write the app's menu can send (gain with the SDK's
0x0300 mask, signal processing on and off)."""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import streamprobe  # noqa: E402
import wsdbg  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "wsdbg-out")


def check(con, label, title=""):
    x, rate = wsdbg.do_pcm(con, 1000, os.path.join(OUT, "cfg_%s.wav" % label), 0)
    con.cmd("tone 1000 4000 255")
    time.sleep(0.3)
    y, _ = wsdbg.do_pcm(con, 1000, os.path.join(OUT, "cfg_%s_tone.wav" % label), 0)
    con.cmd("tone off")
    p = streamprobe.period(x, 1024)
    share = wsdbg.goertzel(y, 1000, rate)
    ok = not p and share > 0.2
    print("%-18s %s, %d distinct, 1 kHz %.1f%%  %s" % (
        title or label, "repeats every %d" % p if p else "moving", len(set(x)), 100 * share,
        "OK" if ok else "<-- BROKEN"), flush=True)


STEPS = [
    ("as started", []),
    ("gain 0x0302", ["reg 4 302 0 r"]),
    ("sp on (1010,0)", ["reg 38 1010 0 r"]),
    ("sp off (1010,1)", ["reg 38 1010 1 r"]),
    ("old gain 2", ["reg 4 2 0 r"]),
]


def main():
    con = wsdbg.Console(sys.argv[1])
    for n, (label, cmds) in enumerate(STEPS):
        for c in cmds:
            print("  > %s  %s" % (c, con.cmd(c)[4].strip()), flush=True)
        time.sleep(0.3)
        check(con, "%d" % n, label)


if __name__ == "__main__":
    main()
