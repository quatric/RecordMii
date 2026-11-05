#!/usr/bin/env python3
"""Reads the Wii Speak's registers back from a running RecordMii.

    regprobe.py HOST

Checks that the start-up settings stuck (sampler state, frequency, gain,
mute) and polls the DSP's level registers in silence and with a 1 kHz tone
on the TV.  A running DSP reports levels that move with the sound; a device
that only echoes its commands, or a DSP that is not running, does not."""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wsdbg  # noqa: E402

READS = [
    ("sampler state", "01 0 0"),
    ("sampler freq ", "03 0 0"),
    ("sampler gain ", "05 0 0"),
    ("sampler mute ", "0d 0 0"),
    ("ec state     ", "15 0 0"),
    ("sp 1010      ", "39 1010 0"),
]
LEVELS = [("sp sout 2004", "39 2004 0"), ("sp rin  200d", "39 200d 0")]


def reg(con, args):
    return con.cmd("reg %s r" % args)[4].strip()


def poll(con, label, n=8):
    print("  -- %s --" % label)
    for _ in range(n):
        print("    " + "   ".join("%s: %s" % (name, reg(con, a)) for name, a in LEVELS))
        time.sleep(0.25)


def main():
    con = wsdbg.Console(sys.argv[1])
    print(con.cmd("stats")[1].splitlines()[0])
    for name, a in READS:
        print("  %s  %s" % (name, reg(con, a)))
    # A register that does not exist: shows what an echo looks like.
    print("  bogus reg 7f  %s" % reg(con, "7f 1234 5678"))
    poll(con, "silence")
    con.cmd("tone 1000 4000 255")
    time.sleep(0.3)
    poll(con, "1 kHz tone")
    con.cmd("tone off")


if __name__ == "__main__":
    main()
