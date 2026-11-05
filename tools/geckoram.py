#!/usr/bin/env python3
"""Reads Wii RAM through a USB Gecko and the Gecko code handler (WiiRD
protocol), while a game runs.  Read-only: it never writes memory.

    geckoram.py [DEV] ping
    geckoram.py [DEV] dump START END OUT.bin
    geckoram.py [DEV] peek ADDR [LEN]

DEV defaults to /dev/cu.usbserial-GECKUSB0."""

import os
import select
import sys
import termios
import time

ACK, RETRY, FAIL = 0xAA, 0xBB, 0xCC
PACKET = 0xF800


class Gecko:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = 0; a[1] = 0; a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def write(self, b):
        b = bytes(b)
        while b:
            select.select([], [self.fd], [], 5)
            n = os.write(self.fd, b)
            b = b[n:]

    def read(self, n, timeout=5.0):
        out = bytearray()
        end = time.time() + timeout
        while len(out) < n:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("got %d of %d bytes" % (len(out), n))
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    out += os.read(self.fd, n - len(out))
                except BlockingIOError:
                    pass
        return bytes(out)

    def version(self):
        self.write([0x99])
        return self.read(1)[0]

    def status(self):
        self.write([0x50])
        return self.read(1)[0]

    def dump(self, start, end, progress=False):
        self.write([0x04])
        if self.read(1)[0] != ACK:
            raise IOError("no ACK for dump")
        self.write(start.to_bytes(4, "big") + end.to_bytes(4, "big"))
        out = bytearray()
        total = end - start
        while len(out) < total:
            n = min(PACKET, total - len(out))
            out += self.read(n, 10)
            self.write([ACK])
            if progress:
                print("\r  %d / %d KB" % (len(out) >> 10, total >> 10), end="", file=sys.stderr, flush=True)
        if progress:
            print(file=sys.stderr)
        return bytes(out)


def main():
    args = sys.argv[1:]
    dev = args.pop(0) if args and args[0].startswith("/dev/") else "/dev/cu.usbserial-GECKUSB0"
    g = Gecko(dev)
    cmd = args[0]
    if cmd == "ping":
        print("version 0x%02x" % g.version())
        st = g.status()
        print("status %d (%s)" % (st, {0: "running", 1: "paused", 2: "breakpoint", 3: "loader"}.get(st, "?")))
    elif cmd == "dump":
        s, e = int(args[1], 16), int(args[2], 16)
        t = time.time()
        data = g.dump(s, e, True)
        open(args[3], "wb").write(data)
        print("%d bytes in %.1f s -> %s" % (len(data), time.time() - t, args[3]))
    elif cmd == "peek":
        a = int(args[1], 16)
        n = int(args[2], 16) if len(args) > 2 else 0x40
        data = g.dump(a, a + n)
        for i in range(0, n, 16):
            print("%08x  %s" % (a + i, data[i:i + 16].hex(" ")))


if __name__ == "__main__":
    main()
