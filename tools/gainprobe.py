#!/usr/bin/env python3
"""Sets the Wii Speak's gain the way the SDK encodes it and checks for audio.

    gainprobe.py HOST

PMICSetGain sends register 0x04 with arg1 = 0x0300 | gain, and PMICGetGain
reads register 0x05 with arg1 = 0x0300.  For each gain this reads the gain
back, then records in silence and with a 1 kHz tone on the TV and reports
whether the stream moves and whether the tone is heard."""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import streamprobe  # noqa: E402
import wsdbg  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "wsdbg-out")


def check(con, label):
    x, rate = wsdbg.do_pcm(con, 1000, os.path.join(OUT, "gain_%s.wav" % label), 0)
    con.cmd("tone 1000 4000 255")
    time.sleep(0.3)
    y, _ = wsdbg.do_pcm(con, 1000, os.path.join(OUT, "gain_%s_tone.wav" % label), 0)
    con.cmd("tone off")
    sx, sy = wsdbg.stats(x), wsdbg.stats(y)
    p = streamprobe.period(x, 1024)
    share0, share = wsdbg.goertzel(x, 1000, rate), wsdbg.goertzel(y, 1000, rate)
    print("    %d distinct, %s; rms %.1f -> %.1f, 1 kHz %.1f%% -> %.1f%%%s"
          % (len(set(x)), "repeats every %d" % p if p else "NOT repeating  <-- MOVING",
             sx["rms"], sy["rms"], 100 * share0, 100 * share,
             "  <-- HEARD THE TONE" if share > 3 * share0 + 0.05 else ""), flush=True)


def reg(con, args):
    return con.cmd("reg %s r" % args)[4].strip()


def main():
    con = wsdbg.Console(sys.argv[1])
    print("gain read, old encoding:  " + reg(con, "5 0 0"))
    print("gain read, SDK encoding:  " + reg(con, "5 300 0"))
    print("level:                    " + reg(con, "39 2004 0"))
    print("baseline")
    check(con, "base")
    for g in (0, 1, 2, 3):
        print("gain %d: set %s  read %s" % (g, reg(con, "4 %x 0" % (0x300 | g)), reg(con, "5 300 0")))
        time.sleep(0.3)
        print("    level " + reg(con, "39 2004 0"))
        check(con, "g%d" % g)


if __name__ == "__main__":
    main()
