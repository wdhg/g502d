#!/usr/bin/env python3
"""Dump every onboard memory sector of the G502 to a file, for backup/restore.

The mouse reports sectorCount=16, sectorSize=256 via 0x8100 getInfo. Sector 0 is the
profile directory; sectors 1..3 hold the three onboard profiles. Higher sectors may be
unreadable (they back macros or are simply absent) - those are recorded as missing
rather than treated as an error.

    ./profile_backup.py save backup.bin
    ./profile_backup.py show backup.bin
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from hidpp import Device, HidppError, hexdump, read_sector

SECTOR_SIZE = 256
SECTOR_COUNT = 16


def save(path):
    sectors = {}
    with Device() as dev:
        for s in range(SECTOR_COUNT):
            try:
                sectors[s] = read_sector(dev, s, SECTOR_SIZE)
                print(f"sector {s:2d}: read ok")
            except HidppError as exc:
                print(f"sector {s:2d}: unreadable ({exc})")

    # Fixed-layout blob: 16 sectors, absent ones zero-filled, plus a presence bitmap
    # at the end so `show` can tell "all zeros" from "not present".
    blob = bytearray()
    present = 0
    for s in range(SECTOR_COUNT):
        if s in sectors:
            blob += sectors[s]
            present |= 1 << s
        else:
            blob += b"\x00" * SECTOR_SIZE
    blob += present.to_bytes(2, "big")

    with open(path, "wb") as fh:
        fh.write(blob)
    print(f"\nwrote {len(blob)} bytes to {path} (present bitmap {present:#06x})")


def show(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    present = int.from_bytes(blob[-2:], "big")
    for s in range(SECTOR_COUNT):
        if not present & (1 << s):
            continue
        data = blob[s * SECTOR_SIZE:(s + 1) * SECTOR_SIZE]
        print(f"=== sector {s} ===")
        print(hexdump(data))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "save":
        save(path)
    elif cmd == "show":
        show(path)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
