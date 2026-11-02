#!/usr/bin/env python3
"""Write the 512 KiB 68000 program image of Blood Bros. (set bloodbro) from the ROM zip.

  python3 tools/dump_prg.py roms/bloodbro.zip work/prg.bin

Same layout as src/romset.cpp: two pairs of even/odd byte ROMs, interleaved, 68000 byte order.
Works without a built game (the game cannot be built before the recompiled code exists).
"""
import sys
import zipfile
import zlib

# (name, crc, offset, lane): lane 0 = even (high) bytes, 1 = odd bytes
FILES = [
    ("1.u022.8n", 0xac6719e7, 0x00000, 0),
    ("2.u021.7n", 0x204dca6e, 0x00000, 1),
    ("3.u024.8l", 0x18d3c460, 0x40000, 0),
    ("4.u023.7l", 0xfd951c2c, 0x40000, 1),
]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    prg = bytearray(0x80000)
    with zipfile.ZipFile(sys.argv[1]) as z:
        names = z.namelist()
        for name, crc, base, lane in FILES:
            found = next((n for n in (name, "bloodbro/" + name) if n in names), None)
            if not found:
                sys.exit("not in the zip: " + name)
            data = z.read(found)
            if zlib.crc32(data) != crc:
                print("warning: %s has CRC %08x, expected %08x" % (name, zlib.crc32(data), crc))
            for i, b in enumerate(data[:0x20000]):
                prg[base + 2 * i + lane] = b
    with open(sys.argv[2], "wb") as f:
        f.write(prg)
    print("wrote %s (%d bytes)" % (sys.argv[2], len(prg)))


if __name__ == "__main__":
    main()
