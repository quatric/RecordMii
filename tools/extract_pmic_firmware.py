#!/usr/bin/env python3
"""Extract the Wii Speak (P-Mic) device firmware blob from the RVL SDK's pmic.a.

The PMIC library uploads PMIC_FIRM_DATA to the Wii Speak over bulk endpoint
0x02 every time the device is opened; the mic will not stream audio until it
has been booted this way.  The blob is Nintendo's, so it is not shipped with
this project -- extract it from your own SDK copy and drop it on the SD card.

Usage:
    ./extract_pmic_firmware.py /path/to/RVL_SDK/RVL/lib/pmic.a -o pmic_firm.bin
"""

import argparse
import struct
import sys

FIRM_SYMBOL = "PMIC_FIRM_DATA"


def read_archive_members(blob):
    """Yield (name, data) for each member of a SysV/GNU ar archive."""
    if blob[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive")

    long_names = b""
    off = 8
    while off + 60 <= len(blob):
        header = blob[off:off + 60]
        if header[58:60] != b"`\n":
            raise ValueError(f"bad ar header at offset {off}")
        name = header[0:16].decode("ascii", "replace").rstrip()
        size = int(header[48:58].decode("ascii").strip())
        body = blob[off + 60:off + 60 + size]

        if name == "//":
            long_names = body
        elif name.startswith("/") and name[1:].isdigit():
            start = int(name[1:])
            end = long_names.find(b"/", start)
            yield long_names[start:end].decode("ascii"), body
        elif name not in ("/", "/SYM64/"):
            yield name.rstrip("/"), body

        off += 60 + size + (size & 1)


def elf_find_symbol(elf, want):
    """Return the file offset and size of `want` in a big-endian ELF32 object."""
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        raise ValueError("not a big-endian ELF32 object")

    e_shoff, = struct.unpack_from(">I", elf, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", elf, 0x2E)

    sections = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        # name, type, flags, addr, offset, size, link, info, align, entsize
        sections.append(struct.unpack_from(">10I", elf, base))

    shstr = sections[e_shstrndx][4]

    def sname(sh):
        end = elf.index(b"\0", shstr + sh[0])
        return elf[shstr + sh[0]:end].decode()

    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]][4]
        for off in range(sh[4], sh[4] + sh[5], sh[9]):
            st_name, st_value, st_size, _info, _other, st_shndx = \
                struct.unpack_from(">IIIBBH", elf, off)
            end = elf.index(b"\0", strtab + st_name)
            if elf[strtab + st_name:end].decode() == want:
                if st_shndx == 0 or st_shndx >= len(sections):
                    raise ValueError(f"{want} has no section")
                return sections[st_shndx][4] + st_value, st_size

    raise KeyError(want)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", help="path to pmic.a (or pmicD.a) from the RVL SDK")
    ap.add_argument("-o", "--output", default="pmic_firm.bin")
    args = ap.parse_args()

    with open(args.archive, "rb") as fh:
        blob = fh.read()

    for name, member in read_archive_members(blob):
        try:
            offset, size = elf_find_symbol(member, FIRM_SYMBOL)
        except (ValueError, KeyError):
            continue

        if size == 0:
            sys.exit(f"{FIRM_SYMBOL} in {name} is empty")

        with open(args.output, "wb") as out:
            out.write(member[offset:offset + size])
        print(f"{FIRM_SYMBOL}: {size} bytes from {name} -> {args.output}")
        return

    sys.exit(f"{FIRM_SYMBOL} not found in {args.archive}")


if __name__ == "__main__":
    main()
