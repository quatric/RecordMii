#!/usr/bin/env python3
"""Reads the Wii Speak Channel's PMIC driver state through a USB Gecko.

    pmicpeek.py [R13]

Run while the Wii Speak Channel is up with the Gecko code handler hooked.
R13 is the game's small-data base (0x80563100 for the copy examined).  Every
block is read twice and only trusted when both reads agree, because long
Gecko reads drop bytes.  Prints the driver globals, each isochronous
transfer's packet size table (length and OHCI condition code), and any
10-byte PMIC command (0x9Axx) found in the buffers the driver points at."""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geckoram  # noqa: E402

GLOBALS = {"__state": -21784, "__fd": -21644, "__iso_count": -21728}


def rd(g, a, n, tries=4):
    for _ in range(tries):
        x = g.dump(a, a + n)
        if x == g.dump(a, a + n):
            return x
    return None


def u32(b, o):
    return int.from_bytes(b[o:o + 4], "big")


def sizes(b, n):
    out = []
    for i in range(n):
        v = int.from_bytes(b[2 * i:2 * i + 2], "big")
        out.append("%2d/cc%X" % (v & 0xFFF, v >> 12))
    return " ".join(out)


def main():
    r13 = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x80563100
    g = geckoram.Gecko("/dev/cu.usbserial-GECKUSB0")
    lo = r13 - 21800
    blk = rd(g, lo, 0xC0)
    if not blk:
        sys.exit("globals unstable")
    for name, off in GLOBALS.items():
        print("%-12s %08x" % (name, u32(blk, r13 + off - lo)))
    ptrs = sorted({u32(blk, i) for i in range(0, len(blk), 4) if 0x90000000 <= u32(blk, i) < 0x94000000})
    print("MEM2 pointers in the globals: " + " ".join("%08x" % p for p in ptrs))

    # Isochronous descriptors look like {buf = self + 0x20, npkts, sizes}.
    seen = set()
    for p in ptrs:
        for base in (p, p - 0x20):
            d = rd(g, base, 0x10)
            if not d or base in seen:
                continue
            buf, npk, sz = u32(d, 0), d[4], u32(d, 8)
            if buf == base + 0x20 and 0 < npk <= 16 and 0x90000000 <= sz < 0x94000000:
                seen.add(base)
                kind = "IN " if sz - buf == 0x100 else "OUT" if sz - buf == 0x200 else "?  "
                print("\n%s descriptor %08x: buf %08x, %d packets, sizes %08x" % (kind, base, buf, npk, sz))
                for k in range(5):
                    t = rd(g, sz, 32)
                    data = rd(g, buf, 32)
                    print("  sizes %s | data %s" % (sizes(t, npk) if t else "unstable",
                                                    data[:16].hex(" ") if data else "unstable"))
                    time.sleep(0.05)

    # Command packets: 0x9Axx, arg1, arg2, 0, seq -- scan the pointed-at areas.
    print("\ncommand-looking words near the driver's buffers:")
    for p in ptrs:
        area = rd(g, p & ~0xFF, 0x400)
        if not area:
            continue
        for i in range(0, len(area) - 10, 2):
            if area[i] == 0x9A and area[i + 1] < 0x40:
                print("  %08x  %s" % ((p & ~0xFF) + i, area[i:i + 10].hex(" ")))


if __name__ == "__main__":
    main()
