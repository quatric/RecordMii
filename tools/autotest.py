#!/usr/bin/env python3
"""Unattended hardware test: load RecordMii, check the mic hears, leave.

    autotest.py HOST [--dol boot.dol] [-o DIR]
    autotest.py /dev/cu.usbserial-GECKUSB0 ...   everything over a USB Gecko

1. Gets the Wii to the Homebrew Channel: if RecordMii is already running it
   is told to leave (`kill`), then the channel's wiiload port is awaited.
2. Sends the .dol with wiiload, waits for the debug console and arms the
   app's deadman, so it goes back to the channel by itself if this script
   dies or loses the network.
3. Waits for the mic to boot, then pulls audio over TCP: the recorded stream
   in silence, and raw transfers plus the recorded stream while a tone plays
   on the TV.  Everything is written as WAVs to DIR.
4. Exits the app whether or not the mic worked, and checks that the Homebrew
   Channel comes back.

Exit status: 0 audio heard, 1 no audio, 2 the Wii could not be driven.
"""

import argparse
import glob
import json
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wsdbg  # noqa: E402

HBC_PORT = 4299
DEADMAN_MS = 60000


def port_open(host, port, timeout=2.0):
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False


def wait_port(host, port, secs):
    end = time.time() + secs
    while time.time() < end:
        if port_open(host, port):
            return True
        time.sleep(2)
    return False


def say(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def gecko_console(dev, secs):
    """The app's console over the Gecko, once it answers."""
    end = time.time() + secs
    while time.time() < end:
        try:
            return wsdbg.Console(dev, timeout=3)
        except (OSError, TimeoutError):
            time.sleep(1)
    return None


def to_loader(host):
    """Returns True once the Homebrew Channel is listening."""
    if host.startswith("/dev/"):
        con = gecko_console(host, 4)
        if con:
            say("RecordMii is running; sending kill over the Gecko")
            print("  " + con.cmd("kill")[4])
            con.close()
            time.sleep(15)
        return True
    if port_open(host, HBC_PORT):
        return True
    if port_open(host, wsdbg.PORT):
        say("RecordMii is running; sending kill")
        try:
            con = wsdbg.Console(host, timeout=10)
            print("  " + con.cmd("kill")[4])
            con.close()
        except (OSError, ConnectionError) as e:
            print("  kill failed: %s" % e)
    say("waiting for the Homebrew Channel")
    return wait_port(host, HBC_PORT, 120)


def gecko_port():
    """A USB Gecko's serial port on this Mac, if one is attached."""
    ports = glob.glob("/dev/tty.usbserial-GECK*") + glob.glob("/dev/tty.usbserial-*")
    return ports[0] if ports else None


def wiiload_gecko(dol):
    """Loads over a USB Gecko instead.  The Homebrew Channel only looks for
    one in memory card slot B, and only when it starts."""
    port = gecko_port()
    if not port:
        return False
    say("trying the USB Gecko on %s" % port)
    r = subprocess.run(["wiiload", dol], env=dict(os.environ, WIILOAD=port),
                       capture_output=True, text=True, timeout=300)
    return r.returncode == 0 and "failed" not in r.stdout + r.stderr


def wiiload(host, dol, args=()):
    if host.startswith("/dev/"):
        r = subprocess.run(["wiiload", dol], env=dict(os.environ, WIILOAD=host),
                           capture_output=True, text=True, timeout=300)
        out = (r.stdout + r.stderr).replace("\r", "\n")
        return r.returncode == 0 and "failed" not in out
    for attempt in range(3):
        if not wait_port(host, HBC_PORT, 60):
            break
        time.sleep(1)  # the port opens a moment before the channel listens
        r = subprocess.run(["wiiload", dol] + list(args), env=dict(os.environ, WIILOAD="tcp:" + host),
                           capture_output=True, text=True, timeout=120)
        if r.returncode == 0 and "failed" not in r.stdout + r.stderr:
            return True
        say("wiiload attempt %d failed: %s" % (attempt + 1, (r.stdout + r.stderr).strip().splitlines()[-1:]))
        time.sleep(5)
    return wiiload_gecko(dol)


BOOT_DONE = ("ready after", "never accepted", "Press HOME", "No microphone", "WS_Open failed",
             "USB mic:", "UM_Start failed")


def wait_boot(con, secs):
    """Collects the log until the mic has booted (or startup gave up)."""
    log = ""
    end = time.time() + secs
    while time.time() < end:
        log += con.cmd("log")[1] + "\n"
        if any(k in log for k in BOOT_DONE):
            # The sampler is switched on a moment after the mic is ready.
            for _ in range(20):
                if "sampler on" in log or "Press HOME" in log or "USB mic:" in log:
                    break
                time.sleep(0.5)
                log += con.cmd("log")[1] + "\n"
            return log, True
        time.sleep(1)
    return log, False


def heard(stats_quiet, stats_tone, tone_share):
    """Decides whether the mic picked anything up.

    Real audio is smooth (rough well below 1.4, which is white noise) and not
    a constant; a tone the TV plays should also stand out of the spectrum."""
    live = stats_quiet["n"] > 0 and stats_quiet["rms"] > 2 and stats_quiet["rough"] < 1.2
    tone = tone_share > 0.2 and stats_tone["rms"] > stats_quiet["rms"] * 1.5
    return live or tone, dict(live=live, tone=tone)


def mic_test(con, out, hz):
    summary = {}
    ok, text, *_ = con.cmd("stats")
    summary["stats_before"] = text
    print(text)
    # Raw transfers are the Wii Speak's; a USB mic only has the recording.
    usb = text.startswith("usb mic")

    # Raw transfers first: they are the smaller download, and they came
    # through when a 3 s recording stalled.
    if not usb:
        say("raw transfers of the room")
        wsdbg.do_cap(con, 1500, out, hz, "room")
    say("recording 1.5 s of the room")
    xq, rate = wsdbg.do_pcm(con, 1500, os.path.join(out, "room.wav"), hz)
    sq = wsdbg.stats(xq)

    say("raw transfers + recording with a %d Hz tone on the TV" % hz)
    print(con.cmd("tone %d %d 255" % (hz, 6000))[4])
    time.sleep(0.5)
    if not usb:
        wsdbg.do_cap(con, 1500, out, hz, "tone")
    xt, _ = wsdbg.do_pcm(con, 1500, os.path.join(out, "tone.wav"), hz)
    con.cmd("tone off")
    st = wsdbg.stats(xt)
    share = wsdbg.goertzel(xt, hz, rate)

    ok, text, *_ = con.cmd("stats")
    summary["stats_after"] = text
    print(text)
    summary["status"] = con.cmd("status")[1]
    print(summary["status"])

    yes, why = heard(sq, st, share)
    summary.update(rate=rate, room=sq, tone=st, tone_share=share, heard=yes, why=why)
    return summary


def leave(con, host):
    """Clean exit, logged; then checks the Homebrew Channel is back."""
    say("exiting the app")
    try:
        con.cmd("log")
        print("  " + con.cmd("exit")[4])
        end = time.time() + 15
        while time.time() < end:
            text = con.cmd("log")[1]
            if text:
                print(text)
            time.sleep(0.3)
    except (OSError, ConnectionError):
        pass
    if host.startswith("/dev/"):
        # The channel does not answer over the Gecko, so all we can tell is
        # whether the app has stopped answering.
        time.sleep(5)
        gone = gecko_console(host, 3) is None
        say("app %s" % ("has left" if gone else "is STILL answering"))
        return gone
    back = wait_port(host, HBC_PORT, 90)
    say("Homebrew Channel %s" % ("is back" if back else "did NOT come back"))
    return back


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--dol", default="boot.dol")
    ap.add_argument("--hz", type=int, default=1000)
    ap.add_argument("-o", default=os.path.join("wsdbg-out", time.strftime("run-%Y%m%d-%H%M%S")))
    a = ap.parse_args()
    os.makedirs(a.o, exist_ok=True)
    result = dict(loaded=False, booted=False, heard=False, back_in_loader=False)

    def finish(code):
        with open(os.path.join(a.o, "summary.json"), "w") as f:
            json.dump(result, f, indent=2, default=str)
        say("summary in %s/summary.json" % a.o)
        sys.exit(code)

    if not to_loader(a.host) and not gecko_port():
        say("the Wii is not reachable (no Homebrew Channel, no app, no USB Gecko)")
        finish(2)

    say("sending %s" % a.dol)
    if not wiiload(a.host, a.dol):
        say("wiiload failed")
        finish(2)
    result["loaded"] = True

    say("waiting for the debug console")
    if a.host.startswith("/dev/"):
        con = gecko_console(a.host, 150)
    elif wait_port(a.host, wsdbg.PORT, 150):
        con = wsdbg.Console(a.host)
    else:
        con = None
    if not con:
        say("the console never came up")
        finish(2)
    print("  " + con.cmd("deadman %d" % DEADMAN_MS)[4])

    log, booted = wait_boot(con, 90)
    with open(os.path.join(a.o, "boot.log"), "w") as f:
        f.write(log)
    print(log)
    result["booted"] = booted

    if booted and "Press HOME" not in log:
        summary = mic_test(con, a.o, a.hz)
        result.update(summary)
        say("mic %s (%s)" % ("HEARD audio" if summary["heard"] else "heard NOTHING", summary["why"]))
    else:
        say("startup did not reach the mic test")

    with open(os.path.join(a.o, "full.log"), "w") as f:
        f.write(con.cmd("log all")[1])

    result["back_in_loader"] = leave(con, a.host)
    finish(0 if result["heard"] else 1)


if __name__ == "__main__":
    main()
