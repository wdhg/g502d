#!/usr/bin/env python3
"""Watch every interface of a device at once and name the events each button sends.

A mouse usually presents several evdev nodes, and buttons the mouse interface
does not claim often arrive on one of the others as a keystroke. Watching them
together is the only way to see which one a given button lands on - or to
establish that it lands on none of them, which means the firmware is handling
it internally.

    ./tools/identify_buttons.py razer          # every node whose name matches
    ./tools/identify_buttons.py g502
    ./tools/identify_buttons.py --list

Stop any daemon that grabs these devices first, or the grab will starve this
tool of events. This only reads; it never grabs, and it changes nothing.
"""

import argparse
import select
import struct
import sys

EVENT_FORMAT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)

EV_SYN, EV_KEY, EV_REL, EV_ABS, EV_MSC = 0x00, 0x01, 0x02, 0x03, 0x04

TYPE_NAMES = {EV_SYN: "EV_SYN", EV_KEY: "EV_KEY", EV_REL: "EV_REL",
              EV_ABS: "EV_ABS", EV_MSC: "EV_MSC", 0x11: "EV_LED", 0x14: "EV_REP"}

# The mouse button block, plus the keyboard codes a mouse is most likely to send.
BUTTON_NAMES = {
    0x110: "BTN_LEFT", 0x111: "BTN_RIGHT", 0x112: "BTN_MIDDLE",
    0x113: "BTN_SIDE", 0x114: "BTN_EXTRA", 0x115: "BTN_FORWARD",
    0x116: "BTN_BACK", 0x117: "BTN_TASK",
}

REL_NAMES = {0x00: "REL_X", 0x01: "REL_Y", 0x06: "REL_HWHEEL", 0x08: "REL_WHEEL",
             0x0b: "REL_WHEEL_HI_RES", 0x0c: "REL_HWHEEL_HI_RES"}

MSC_NAMES = {0x04: "MSC_SCAN"}


def read_devices():
    """Parse /proc/bus/input/devices into (name, path) pairs."""
    devices, name = [], None
    with open("/proc/bus/input/devices") as handle:
        for raw in handle:
            line = raw.strip()
            if line.startswith('N: Name="'):
                name = line[9:-1]
            elif line.startswith("H: Handlers="):
                for handler in line[12:].split():
                    if handler.startswith("event"):
                        devices.append((name or "?", "/dev/input/" + handler))
    return devices


def code_name(etype, code):
    if etype == EV_KEY:
        if code in BUTTON_NAMES:
            return BUTTON_NAMES[code]
        if 0x110 <= code <= 0x11f:
            return f"BTN_MOUSE+{code - 0x110}"
        return f"key {code}"
    if etype == EV_REL:
        return REL_NAMES.get(code, f"rel {code}")
    if etype == EV_MSC:
        return MSC_NAMES.get(code, f"msc {code}")
    return str(code)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("match", nargs="?",
                        help="case-insensitive substring of the device name")
    parser.add_argument("--list", action="store_true", help="list devices and exit")
    parser.add_argument("--motion", action="store_true",
                        help="also show pointer motion (very noisy)")
    args = parser.parse_args()

    devices = read_devices()
    if args.list:
        for name, path in devices:
            print(f"{path:22} {name}")
        return
    if not args.match:
        parser.error("give a name to match, or --list")

    wanted = [(n, p) for n, p in devices if args.match.lower() in n.lower()]
    if not wanted:
        sys.exit(f"no input device matching {args.match!r} - try --list")

    streams = {}
    for name, path in wanted:
        try:
            streams[open(path, "rb", buffering=0)] = (name, path)
        except PermissionError:
            sys.exit(f"permission denied on {path} - try: sudo {' '.join(sys.argv)}")
        except OSError as exc:
            print(f"skipping {path}: {exc}", file=sys.stderr)

    if not streams:
        sys.exit("nothing could be opened")

    width = max(len(p) for _, p in streams.values())
    print("watching:")
    for name, path in streams.values():
        print(f"  {path:{width}}  {name}")
    print("\npress each button in turn. a button that prints nothing on any node is")
    print("being handled inside the mouse and never reaches the host. Ctrl-C to stop.\n")

    seen = {}
    try:
        while True:
            ready, _, _ = select.select(list(streams), [], [])
            for stream in ready:
                data = stream.read(EVENT_SIZE)
                if not data or len(data) < EVENT_SIZE:
                    continue
                _, _, etype, code, value = struct.unpack(EVENT_FORMAT, data)
                if etype == EV_SYN:
                    continue
                if etype == EV_REL and not args.motion and code in (0x00, 0x01):
                    continue

                name, path = streams[stream]
                label = code_name(etype, code)
                detail = f"0x{value:08x}" if etype == EV_MSC else str(value)
                print(f"{path:{width}}  {TYPE_NAMES.get(etype, etype):<7} "
                      f"{label:<18} {detail}")

                if etype == EV_KEY and value == 1:
                    seen.setdefault(label, path)
    except KeyboardInterrupt:
        pass
    finally:
        for stream in streams:
            stream.close()

    if seen:
        print("\nbuttons seen:")
        for label, path in sorted(seen.items()):
            print(f"  {label:<18} on {path}")


if __name__ == "__main__":
    main()
