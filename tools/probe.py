#!/usr/bin/env python3
"""Hunts for the command that makes the Wii Speak actually sample.

    probe.py HOST [--dol boot.dol] [--no-load]
    probe.py HOST --matrix          liveness under each IOS / stream setup

Loads RecordMii (unless --no-load), then reads back the mic's registers and
tries each switch in turn.  After every step it captures raw transfers and
counts how many distinct packets arrived: a mic that is sampling never sends
the same 32 bytes twice in a row, while an idle one repeats a single frame.
Each step is also announced on this Mac's speaker, to match against the
Wii Speak's LED.  Leaves the app at the end.
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autotest  # noqa: E402
import wsdbg  # noqa: E402

# (label, console command); `reg R A1 A2 r` sends a raw command and reads the reply.
READS = [
    ("sampler state", "reg 1 0 0 r"),
    ("sampler freq", "reg 3 0 0 r"),
    ("sampler gain", "reg 5 0 0 r"),
    ("sampler mute", "reg d 0 0 r"),
    ("echo state", "reg 15 0 0 r"),
    ("sp state", "reg 39 1010 0 r"),
]

STEPS = [
    ("baseline", None),
    ("sampler on, reply read", "reg 0 1 0 r"),
    ("mute off", "reg c 0 0 r"),
    ("echo canceller reset pulse", ["reg 14 1 0 r", "reg 14 0 0 r"]),
    ("signal processing on", "sp 1"),
    ("signal processing off", "sp 0"),
    ("gain 36 dB", "gain 3"),
    ("sampler off then on", ["reg 0 0 0 r", "reg 0 1 0 r", "reg c 0 0 r"]),
]


def speak(text):
    subprocess.run(["say", text], check=False)


def liveness(con):
    """Distinct 32-byte packets in 0.5 s of raw transfers (~496 if live)."""
    ok, _, blob, info, st = con.cmd("cap 500")
    if not ok or not blob:
        return 0, 0, st
    recs = wsdbg.parse_records(blob, info[0])
    pkts = [r[2][i * 32:(i + 1) * 32] for r in recs if r[0] >= 0 for i in range(8)]
    return len(set(pkts)), len(pkts), st


MATRIX = [
    ["ios=56", "slots=2", "out=1"],
    ["ios=56", "slots=1", "out=1"],
    ["ios=56", "slots=2", "out=0"],
    ["ios=38", "slots=2", "out=1"],
    ["ios=36", "slots=2", "out=1"],
]


def matrix(host, dol):
    """Boots the app once per setup and reports whether frames move."""
    results = []
    for args in MATRIX:
        print("\n== %s ==" % " ".join(args), flush=True)
        if not autotest.to_loader(host) or not autotest.wiiload(host, dol, args):
            print("  could not load")
            results.append((args, "no load"))
            continue
        if not autotest.wait_port(host, wsdbg.PORT, 150):
            print("  console never came up")
            results.append((args, "no console"))
            continue
        con = wsdbg.Console(host)
        con.cmd("deadman 60000")
        log, _ = autotest.wait_boot(con, 90)
        for ln in log.splitlines():
            if ln.startswith(("USB:", "Args:", "  ready", "  sampler", "  mute", "  device never")):
                print("  " + ln.strip())
        time.sleep(2)
        verdict = []
        for _ in range(2):
            distinct, total, st = liveness(con)
            verdict.append("%d/%d" % (distinct, total))
            time.sleep(1)
        print("  distinct packets: %s" % ", ".join(verdict), flush=True)
        results.append((args, ", ".join(verdict)))
        print("  " + con.cmd("kill")[4])
        con.close()
        time.sleep(10)
    print("\n== summary ==")
    for args, v in results:
        print("  %-28s %s" % (" ".join(args), v))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--dol", default="boot.dol")
    ap.add_argument("--no-load", action="store_true")
    ap.add_argument("--matrix", action="store_true")
    a = ap.parse_args()

    if a.matrix:
        matrix(a.host, a.dol)
        return

    if not a.no_load:
        if not autotest.to_loader(a.host) or not autotest.wiiload(a.host, a.dol):
            sys.exit("could not load the app")
        if not autotest.wait_port(a.host, wsdbg.PORT, 150):
            sys.exit("console never came up")
    con = wsdbg.Console(a.host)
    print(con.cmd("deadman 120000")[4])
    log, _ = autotest.wait_boot(con, 90)
    print(log.strip().splitlines()[-4:])

    print("\n== registers ==")
    for label, cmd in READS:
        print("  %-14s %-18s %s" % (label, cmd, con.cmd(cmd)[4]))

    print("\n== steps ==")
    for label, cmds in STEPS:
        speak(label)
        for c in ([cmds] if isinstance(cmds, str) else cmds or []):
            print("  > %-16s %s" % (c, con.cmd(c)[4]))
        time.sleep(2)
        distinct, total, st = liveness(con)
        print("  %-28s distinct packets %d of %d  -> %s" % (
            label, distinct, total, "LIVE" if total and distinct > total // 2 else "frozen"))
        print("    sampler state %s" % con.cmd("reg 1 0 0 r")[4])
        time.sleep(2)

    speak("done")
    autotest.leave(con, a.host)


if __name__ == "__main__":
    main()
