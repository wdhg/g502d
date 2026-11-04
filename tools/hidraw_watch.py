#!/usr/bin/env python3
"""Watch raw HID reports from the G502's two USB interfaces at once.

Interface 0 (hidraw0) carries the boot-style mouse report: buttons, X/Y, wheel, AC pan.
Interface 1 (hidraw1) carries HID++ (report IDs 0x10 short / 0x11 long) plus the
keyboard/consumer/system collections the mouse also exposes.

Anything the mouse physically sends shows up here, regardless of EVIOCGRAB, so this
works while moused is running.
"""

import os
import selectors
import sys
import time

PATHS = ["/dev/hidraw0", "/dev/hidraw1"]


def describe(path, buf):
    """Best-effort decode so button presses are readable at a glance."""
    if not buf:
        return ""
    rid = buf[0]
    if path.endswith("hidraw0"):
        # No report ID on interface 0: 16 button bits, then X/Y (16-bit), wheel, pan.
        if len(buf) >= 2:
            btns = buf[0] | (buf[1] << 8)
            pressed = [f"b{i + 1}" for i in range(16) if btns & (1 << i)]
            return f"buttons={btns:#06x} [{' '.join(pressed) or 'none'}]"
        return ""
    if rid == 0x10:
        return "HID++ SHORT"
    if rid == 0x11:
        return "HID++ LONG"
    if rid == 0x01:
        return "keyboard"
    if rid == 0x03:
        return "consumer"
    if rid == 0x04:
        return "system"
    return f"report id {rid:#04x}"


def main():
    sel = selectors.DefaultSelector()
    for path in PATHS:
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError as exc:
            print(f"cannot open {path}: {exc}", file=sys.stderr)
            continue
        sel.register(fd, selectors.EVENT_READ, path)

    if not sel.get_map():
        sys.exit("no hidraw nodes opened")

    print("watching:", ", ".join(PATHS), "- press mouse buttons now", flush=True)
    start = time.monotonic()
    last_buttons = None
    while True:
        for key, _ in sel.select():
            try:
                buf = os.read(key.fd, 64)
            except BlockingIOError:
                continue
            # Interface 0 streams a report per sensor poll; only the button bits
            # matter here, so collapse the motion-only flood.
            if key.data.endswith("hidraw0"):
                buttons = (buf[0] | (buf[1] << 8)) if len(buf) >= 2 else 0
                wheel = buf[6] if len(buf) > 6 else 0
                pan = buf[7] if len(buf) > 7 else 0
                if buttons == last_buttons and not wheel and not pan:
                    continue
                last_buttons = buttons
            hexs = " ".join(f"{b:02x}" for b in buf)
            t = time.monotonic() - start
            name = os.path.basename(key.data)
            print(f"[{t:7.3f}] {name}: {hexs:<60} {describe(key.data, buf)}", flush=True)


if __name__ == "__main__":
    main()
