#!/usr/bin/env python3
"""Listen to HID++ feature 0x8110 MOUSE_BUTTON_SPY.

This feature reports the raw state of every physical button as a bitmap, before the
onboard profile decides what (if anything) to do with it. That makes it the only way
to observe buttons the firmware consumes internally - notably the thumb paddle, which
factory profile 1 binds to the on-device DPI-shift action and therefore never puts on
the wire as a HID report.

Functions (from cvuchener/hidpp, src/libhidpp/hidpp20/IMouseButtonSpy.h):
    0 GetMouseButtonCount
    1 StartMouseButtonSpy
    2 StopMouseButtonSpy
    3 GetMouseButtonMapping
    4 SetMouseButtonMapping
Event 0 carries a big-endian uint16 button mask in the first two parameter bytes.

The spy is volatile: it stops on unplug, and this tool always stops it on exit.

    ./button_spy.py [seconds]
"""

import os
import select
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from hidpp import LONG, SHORT, Device, HidppError

FEATURE_MOUSE_BUTTON_SPY = 0x8110

# Physical button order, recovered from the factory profiles (see tools/profile.py).
BUTTON_NAMES = [
    "left click", "right click", "middle click",
    "thumb rear (back)", "thumb front (forward)", "THUMB PADDLE (sniper)",
    "behind-wheel forward", "behind-wheel back", "profile switch",
    "wheel tilt right", "wheel tilt left",
]


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

    with Device() as dev:
        idx = dev.feature_index(FEATURE_MOUSE_BUTTON_SPY)
        count = dev.request(idx, 0)[0]
        print(f"0x8110 at feature index {idx}, {count} physical buttons", flush=True)

        try:
            mapping = dev.request(idx, 3, long=True)
            print(f"current mapping: {' '.join(f'{b:02x}' for b in mapping[:count])}",
                  flush=True)
        except HidppError as exc:
            print(f"getMouseButtonMapping: {exc}", flush=True)

        dev.request(idx, 1)  # StartMouseButtonSpy
        print(f"spy STARTED - press buttons (listening {duration:.0f}s)\n", flush=True)

        try:
            listen(dev, idx, duration)
        finally:
            try:
                dev.request(idx, 2)  # StopMouseButtonSpy
                print("\nspy STOPPED", flush=True)
            except HidppError as exc:
                print(f"\nfailed to stop spy: {exc} (unplug the mouse to clear)",
                      file=sys.stderr)


def listen(dev, idx, duration):
    start = time.monotonic()
    last_mask = 0
    while True:
        remaining = duration - (time.monotonic() - start)
        if remaining <= 0:
            return
        if not select.select([dev.fd], [], [], remaining)[0]:
            continue
        try:
            buf = os.read(dev.fd, 64)
        except BlockingIOError:
            continue

        # Notification: report 0x10/0x11, our feature index, function nibble 0
        # (event 0) and software id 0.
        if len(buf) < 6 or buf[0] not in (SHORT, LONG):
            continue
        if buf[2] != idx or buf[3] != 0x00:
            continue

        mask = (buf[4] << 8) | buf[5]  # big-endian
        changed = mask ^ last_mask
        last_mask = mask

        t = time.monotonic() - start
        for bit in range(16):
            if not changed & (1 << bit):
                continue
            name = BUTTON_NAMES[bit] if bit < len(BUTTON_NAMES) else f"button {bit + 1}"
            edge = "PRESS  " if mask & (1 << bit) else "release"
            print(f"[{t:6.2f}] {edge} button {bit + 1:2d}  {name}", flush=True)


if __name__ == "__main__":
    main()
