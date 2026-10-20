#!/usr/bin/env python3
"""Client for RecordMii's TCP debug console (source/netdbg.c).

    wsdbg.py HOST help                  any console command, reply printed
    wsdbg.py HOST shell                 interactive
    wsdbg.py HOST log [--follow]
    wsdbg.py HOST cap MS [-o DIR]       raw IN transfers, analysed
    wsdbg.py HOST pcm MS [-o FILE]      what the app records, as a WAV
    wsdbg.py HOST tonetest [--hz 1000]  plays a tone on the TV and checks the
                                        mic hears it, for every decoding
    wsdbg.py HOST get PATH [-o FILE]
    wsdbg.py HOST exitwatch             sends exit, prints the shutdown log

HOST may be HOST:PORT; the port defaults to 4300.
"""

import argparse
import math
import os
import socket
import struct
import sys
import time
import wave

PORT = 4300
PKTS, PKTSIZE = 8, 32
POISON = 0xEE


class Console:
    def __init__(self, host, timeout=30):
        h, _, p = host.partition(":")
        self.sock = socket.create_connection((h, int(p or PORT)), timeout=timeout)
        self.buf = b""
        self.banner = self._line()

    def _fill(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError("console closed the connection")
        self.buf += data

    def _line(self):
        while b"\n" not in self.buf:
            self._fill()
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("latin-1")

    def _bytes(self, n):
        while len(self.buf) < n:
            self._fill()
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def cmd(self, line):
        """Returns (ok, text, binary, binary_header_fields, status_line)."""
        self.sock.sendall(line.encode() + b"\n")
        text, blob, info = [], None, []
        while True:
            ln = self._line()
            if ln.startswith("BIN "):
                fields = ln.split()[1:]
                blob = self._bytes(int(fields[0]))
                info = [int(x) for x in fields[1:]]
            elif ln == "OK" or ln.startswith("OK ") or ln.startswith("ERR"):
                return ln.startswith("OK"), "\n".join(text), blob, info, ln
            else:
                text.append(ln)

    def close(self):
        self.sock.close()


# ---- analysis ---------------------------------------------------------------


def parse_records(blob, recsize):
    recs = []
    for off in range(0, len(blob) - recsize + 1, recsize):
        ret = struct.unpack_from(">i", blob, off)[0]
        sizes = struct.unpack_from(">8H", blob, off + 4)
        data = blob[off + 20 : off + 20 + PKTS * PKTSIZE]
        recs.append((ret, sizes, data))
    return recs


def decode(recs, layout, little):
    out = bytearray()
    for ret, sizes, data in recs:
        if ret < 0:
            continue
        if layout == "packed":
            n = ret if 0 <= ret <= len(data) else min(sum(sizes), len(data))
            out += data[:n - (n & 1)]
        elif layout == "slots":
            for i in range(PKTS):
                n = min(sizes[i], PKTSIZE)
                out += data[i * PKTSIZE : i * PKTSIZE + n - (n & 1)]
        elif layout == "half":
            for i in range(PKTS):
                out += data[i * PKTSIZE : i * PKTSIZE + PKTSIZE // 2]
        elif layout == "all":
            out += data
    n = len(out) // 2
    return list(struct.unpack(("<" if little else ">") + "%dh" % n, bytes(out[: n * 2])))


def stats(x):
    if not x:
        return dict(n=0, rms=0, dc=0, rough=0, peak=0)
    n = len(x)
    dc = sum(x) / n
    ac = [v - dc for v in x]
    rms = math.sqrt(sum(v * v for v in ac) / n)
    diff = math.sqrt(sum((ac[i] - ac[i - 1]) ** 2 for i in range(1, n)) / max(n - 1, 1))
    # Audio is smooth: neighbouring samples are close.  White noise -- which
    # is what the wrong byte order or stale bytes look like -- scores ~1.41.
    rough = diff / rms if rms else 0
    return dict(n=n, rms=rms, dc=dc, rough=rough, peak=max(abs(v) for v in x))


def goertzel(x, freq, rate):
    """Fraction of the signal's AC power within one bin of `freq`."""
    if len(x) < 64 or rate <= 0 or freq >= rate / 2:
        return 0.0
    n = min(len(x), 8192)
    seg = x[:n]
    dc = sum(seg) / n
    k = freq * n / rate
    w = 2 * math.pi * k / n
    coeff = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in seg:
        s0 = (v - dc) + coeff * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    total = sum((v - dc) ** 2 for v in seg) * n / 2
    return power / total if total else 0.0


def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def describe_transfers(recs):
    rets, tables = {}, {}
    for ret, sizes, _ in recs:
        rets[ret] = rets.get(ret, 0) + 1
        tables[sizes] = tables.get(sizes, 0) + 1
    print("  %d transfers" % len(recs))
    print("  ioctl results : " + ", ".join("%d x%d" % kv for kv in sorted(rets.items(), key=lambda kv: -kv[1])[:6]))
    for t, c in sorted(tables.items(), key=lambda kv: -kv[1])[:4]:
        print("  sizes %-36s x%d" % (" ".join(map(str, t)), c))

    # Which bytes of each 32-byte slot IOS actually wrote: the app poisons
    # the buffer with 0xEE before every transfer.
    written = [0] * PKTSIZE
    for _, _, data in recs:
        for i in range(PKTS):
            for j in range(PKTSIZE):
                if data[i * PKTSIZE + j] != POISON:
                    written[j] += 1
    total = len(recs) * PKTS or 1
    row = "".join("#" if c / total > 0.9 else ("+" if c / total > 0.1 else ".") for c in written)
    print("  bytes written per slot (0..31): [%s]  (# always, + sometimes, . never)" % row)


def analyse(recs, rate, tone=None, outdir=None, tag="cap"):
    describe_transfers(recs)
    results = []
    print("\n  %-7s %-3s %7s %8s %7s %6s %6s  %s" % ("layout", "end", "samples", "rms", "dc", "rough", "peak", "tone" if tone else ""))
    for layout in ("slots", "packed", "half", "all"):
        for little in (False, True):
            x = decode(recs, layout, little)
            s = stats(x)
            t = goertzel(x, tone, rate) if tone else None
            results.append((layout, little, s, t, x))
            print("  %-7s %-3s %7d %8.1f %7.1f %6.2f %6d  %s" % (
                layout, "LE" if little else "BE", s["n"], s["rms"], s["dc"], s["rough"], s["peak"],
                ("%5.1f%%" % (100 * t)) if t is not None else ""))
            if outdir:
                write_wav(os.path.join(outdir, "%s_%s_%s.wav" % (tag, layout, "le" if little else "be")), x, rate)
    return results


# ---- commands -----------------------------------------------------------------


def current_rate(con):
    ok, text, *_ = con.cmd("status")
    for line in text.splitlines():
        if line.startswith("rate "):
            return int(line.split()[1])
    return 16000


def do_cap(con, ms, outdir, tone=None, tag="cap"):
    ok, text, blob, info, st = con.cmd("cap %d" % ms)
    if not ok:
        sys.exit(st)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, tag + ".bin"), "wb") as f:
        f.write(blob)
    recs = parse_records(blob, info[0])
    rate = current_rate(con)
    print("%s: %s (rate %d)" % (tag, st, rate))
    return analyse(recs, rate, tone, outdir, tag)


def do_pcm(con, ms, out, tone=None):
    ok, text, blob, info, st = con.cmd("pcm %d" % ms)
    if not ok:
        sys.exit(st)
    rate = info[0] or 16000
    n = len(blob) // 2
    x = list(struct.unpack(">%dh" % n, blob[: n * 2]))
    write_wav(out, x, rate)
    s = stats(x)
    t = goertzel(x, tone, rate) if tone else None
    print("pcm: %s, %d Hz -> %s" % (st, rate, out))
    print("  rms %.1f dc %.1f rough %.2f peak %d%s" % (s["rms"], s["dc"], s["rough"], s["peak"],
          ("  tone %.1f%%" % (100 * t)) if t is not None else ""))
    return x, rate


def tonetest(con, hz, ms, vol, outdir):
    os.makedirs(outdir, exist_ok=True)
    print(con.cmd("stats")[1])
    print("\n== silence ==")
    quiet = do_cap(con, 1500, outdir, hz, "quiet")
    print("\n== tone %d Hz, vol %d ==" % (hz, vol))
    ok, _, _, _, st = con.cmd("tone %d %d %d" % (hz, ms + 2500, vol))
    print(st)
    time.sleep(0.4)
    loud = do_cap(con, ms, outdir, hz, "tone")
    do_pcm(con, 1000, os.path.join(outdir, "tone_recorded.wav"), hz)
    con.cmd("tone off")

    print("\n== verdict ==")
    best = None
    for (lay, le, sq, tq, _), (_, _, sl, tl, _) in zip(quiet, loud):
        gain = sl["rms"] / sq["rms"] if sq["rms"] else float("inf")
        score = (tl or 0) * (1 if sl["rough"] < 1.2 else 0.2)
        print("  %-6s %s  tone share %5.1f%% (silence %5.1f%%)  rms x%.1f  rough %.2f" % (
            lay, "LE" if le else "BE", 100 * (tl or 0), 100 * (tq or 0), gain, sl["rough"]))
        if best is None or score > best[0]:
            best = (score, lay, le)
    print("\n  best match: layout=%s %s" % (best[1], "little-endian" if best[2] else "big-endian"))
    print("  WAVs for every decoding are in %s/" % outdir)


def exitwatch(con):
    ok, text, _, _, st = con.cmd("log")  # move the cursor past old output
    print(con.cmd("exit")[4])
    deadline = time.time() + 20
    try:
        while time.time() < deadline:
            ok, text, _, _, st = con.cmd("log")
            if text:
                print(text)
            time.sleep(0.2)
        print("(console still up after 20 s -- the exit path is stuck; see the last step above)")
    except (ConnectionError, OSError):
        print("(console closed: the app reached the network shutdown)")


def shell(con):
    print(con.banner)
    while True:
        try:
            line = input("wii> ").strip()
        except EOFError:
            break
        if not line:
            continue
        ok, text, blob, info, st = con.cmd(line)
        if text:
            print(text)
        if blob is not None:
            print("(%d bytes of binary data)" % len(blob))
        print(st)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args()

    con = Console(a.host)
    argv = a.command or ["shell"]
    name, rest = argv[0], argv[1:]

    sub = argparse.ArgumentParser(prog=name)
    if name == "shell":
        shell(con)
    elif name == "log":
        sub.add_argument("--follow", action="store_true")
        sub.add_argument("--all", action="store_true")
        o = sub.parse_args(rest)
        print(con.cmd("log all" if o.all else "log")[1])
        while o.follow:
            time.sleep(0.5)
            text = con.cmd("log")[1]
            if text:
                print(text)
    elif name == "cap":
        sub.add_argument("ms", type=int)
        sub.add_argument("-o", default="wsdbg-out")
        sub.add_argument("--hz", type=int)
        o = sub.parse_args(rest)
        do_cap(con, o.ms, o.o, o.hz)
    elif name == "pcm":
        sub.add_argument("ms", type=int)
        sub.add_argument("-o", default="wsdbg-out/pcm.wav")
        sub.add_argument("--hz", type=int)
        o = sub.parse_args(rest)
        os.makedirs(os.path.dirname(o.o) or ".", exist_ok=True)
        do_pcm(con, o.ms, o.o, o.hz)
    elif name == "tonetest":
        sub.add_argument("--hz", type=int, default=1000)
        sub.add_argument("--ms", type=int, default=2000)
        sub.add_argument("--vol", type=int, default=255)
        sub.add_argument("-o", default="wsdbg-out")
        o = sub.parse_args(rest)
        tonetest(con, o.hz, o.ms, o.vol, o.o)
    elif name == "get":
        sub.add_argument("path")
        sub.add_argument("-o")
        o = sub.parse_args(rest)
        ok, _, blob, _, st = con.cmd("get " + o.path)
        if not ok:
            sys.exit(st)
        out = o.o or os.path.basename(o.path)
        with open(out, "wb") as f:
            f.write(blob)
        print("%s -> %s (%d bytes)" % (st, out, len(blob)))
    elif name == "exitwatch":
        exitwatch(con)
    else:
        ok, text, blob, info, st = con.cmd(" ".join(argv))
        if text:
            print(text)
        if blob is not None:
            print("(%d bytes of binary data)" % len(blob))
        print(st)
        sys.exit(0 if ok else 1)
    con.close()


if __name__ == "__main__":
    main()
