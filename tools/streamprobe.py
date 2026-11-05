#!/usr/bin/env python3
"""Records 1.5 s from a running RecordMii and says whether the stream moves.

    streamprobe.py HOST

Prints the number of distinct sample values and the shortest period (in
samples) after which the recording repeats itself exactly, if any.  A live mic
has hundreds of distinct values and no short period; the frozen stream this
tool was written for repeats one transfer's worth of samples forever."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wsdbg  # noqa: E402


def period(x, limit=4096):
    n = len(x)
    for p in range(1, min(limit, n // 2)):
        same = sum(1 for i in range(n - p) if x[i] == x[i + p])
        if same >= 0.99 * (n - p):
            return p
    return None


def main():
    con = wsdbg.Console(sys.argv[1])
    print(con.cmd("stats")[1].splitlines()[0])
    x, rate = wsdbg.do_pcm(con, 1500, os.path.join(os.path.dirname(__file__), "..", "wsdbg-out", "probe.wav"), 0)
    st = wsdbg.stats(x)
    print("  %d samples at %d Hz, %d distinct values, rms %.1f, dc %.1f" % (len(x), rate, len(set(x)), st["rms"], st["dc"]))
    p = period(x)
    print("  repeats exactly every %s samples" % p if p else "  no exact repetition  <-- MOVING")

    # Then the same with a 1 kHz tone on the TV: is it in the recording?
    print(con.cmd("tone 1000 6000 255")[4])
    import time
    time.sleep(0.6)
    y, _ = wsdbg.do_pcm(con, 1500, os.path.join(os.path.dirname(__file__), "..", "wsdbg-out", "probe_tone.wav"), 0)
    con.cmd("tone off")
    sy = wsdbg.stats(y)
    share, share0 = wsdbg.goertzel(y, 1000, rate), wsdbg.goertzel(x, 1000, rate)
    heard = share > 0.2 and sy["rms"] > st["rms"] * 1.5
    print("  1 kHz share %.1f%% with the tone, %.1f%% without; rms %.1f -> %.1f  %s"
          % (100 * share, 100 * share0, st["rms"], sy["rms"], "<-- HEARD THE TONE" if heard else "(tone not heard)"))


if __name__ == "__main__":
    main()
