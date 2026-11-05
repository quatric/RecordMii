#!/usr/bin/env python3
"""Finds an SDK library's functions in a Wii RAM dump.

    sdkfind.py LIB.a DUMP.bin [BASE]

Each function's code is matched with linker-filled fields masked out
(branch targets, address halves, small-data offsets).  For every match it
prints the address and resolves the small-data (r13/r2) and absolute
address references the function makes, so that a library's globals can be
read out of the dump by name."""

import re
import subprocess
import sys

OD = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump"
INSN = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ){3}[0-9a-f]{2})\s")
RELOC = re.compile(r"^\s*([0-9a-f]+): (R_PPC_\S+)\s+(\S+)")
FUNC = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
MASKS = {"R_PPC_REL24": 0x03FFFFFC, "R_PPC_ADDR16_HA": 0xFFFF, "R_PPC_ADDR16_LO": 0xFFFF,
         "R_PPC_ADDR16_HI": 0xFFFF, "R_PPC_EMB_SDA21": 0x001FFFFF, "R_PPC_REL14": 0xFFFC,
         "R_PPC_ADDR32": 0xFFFFFFFF}


def functions(lib):
    out = subprocess.run([OD, "-dr", lib], capture_output=True, text=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            cur = m.group(2)
            funcs[cur] = {"start": int(m.group(1), 16), "words": [], "relocs": {}}
            continue
        if cur is None:
            continue
        m = INSN.match(line)
        if m:
            funcs[cur]["words"].append((int(m.group(1), 16), bytes.fromhex(m.group(2).replace(" ", ""))))
            continue
        m = RELOC.match(line)
        if m:
            off = int(m.group(1), 16)
            funcs[cur]["relocs"][off & ~3] = (m.group(2), m.group(3), off)
    return funcs


def pattern(f):
    parts = []
    for off, w in f["words"]:
        r = f["relocs"].get(off)
        if r and r[0] in MASKS:
            mask = MASKS[r[0]]
            word = int.from_bytes(w, "big")
            for i in range(4):
                sh = 24 - 8 * i
                if (mask >> sh) & 0xFF:
                    parts.append(b"." if (mask >> sh) & 0xFF == 0xFF else b".")
                else:
                    parts.append(re.escape(bytes([(word >> sh) & 0xFF])))
        else:
            parts.append(re.escape(w))
    return re.compile(b"".join(parts), re.S)


def main():
    lib, dump = sys.argv[1], sys.argv[2]
    base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x80000000
    data = open(dump, "rb").read()
    funcs = functions(lib)
    syms = {}
    for name, f in funcs.items():
        if len(f["words"]) < 8:
            continue
        hits = [m.start() for m in pattern(f).finditer(data)]
        if len(hits) != 1:
            print("%-28s %s" % (name, "%d matches" % len(hits) if hits else "not found"))
            continue
        addr = base + hits[0]
        print("%-28s %08x" % (name, addr))
        f0 = f["words"][0][0]
        ha = {}
        for off, w in f["words"]:
            r = f["relocs"].get(off)
            if not r:
                continue
            word = int.from_bytes(data[hits[0] + off - f0: hits[0] + off - f0 + 4], "big")
            kind, sym = r[0], r[1]
            if kind == "R_PPC_EMB_SDA21":
                reg = (word >> 16) & 0x1F
                d = word & 0xFFFF
                d = d - 0x10000 if d & 0x8000 else d
                syms.setdefault(sym, ("r%d" % reg, d))
            elif kind == "R_PPC_ADDR16_HA":
                ha[sym] = word & 0xFFFF
            elif kind == "R_PPC_ADDR16_LO" and sym in ha:
                lo = word & 0xFFFF
                lo = lo - 0x10000 if lo & 0x8000 else lo
                syms.setdefault(sym, ((ha[sym] << 16) + lo) & 0xFFFFFFFF)
            elif kind == "R_PPC_REL24":
                d = word & 0x03FFFFFC
                d = d - 0x04000000 if d & 0x02000000 else d
                syms.setdefault(sym, ("call", (addr + off - f0 + d) & 0xFFFFFFFF))
    print("\n== referenced symbols ==")
    for s, v in sorted(syms.items(), key=lambda kv: str(kv[1])):
        if isinstance(v, int):
            print("  %-26s %08x" % (s, v))
        elif v[0] == "call":
            print("  %-26s call %08x" % (s, v[1]))
        else:
            print("  %-26s %s%+d" % (s, v[0], v[1]))


if __name__ == "__main__":
    main()
