#!/usr/bin/env python3
"""Count BTN_LEFT press/release pairs on an evdev node.

This is the layer *below* the compositor: it reports exactly what the kernel
put on the device, before libinput, the toolkit or the app get a say. Compare
it with tools/clicktest.html - if this says one click and the browser says two,
the doubling is happening above evdev.

    ./tools/clickcount.py                 # auto-pick the virtual G502
    ./tools/clickcount.py /dev/input/event18
    ./tools/clickcount.py --list

Needs read access to /dev/input, so usually sudo. Ctrl-C to stop; a summary is
printed on exit. This only reads - it never grabs the device, so it cannot
interfere with the daemon or steal your clicks.
"""

import argparse
import struct
import sys

EV_KEY = 0x01
EV_SYN = 0x00
BTN_LEFT = 0x110

# struct input_event: struct timeval (2 x long), __u16 type, __u16 code, __s32 value
EVENT_FORMAT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)

# Anything faster than this is not a human clicking twice.
DUPLICATE_MS = 80.0


def read_devices():
    """Parse /proc/bus/input/devices into (name, evdev path) pairs."""
    devices = []
    name = None
    try:
        with open("/proc/bus/input/devices") as handle:
            for raw in handle:
                line = raw.strip()
                if line.startswith('N: Name="'):
                    name = line[9:-1]
                elif line.startswith("H: Handlers="):
                    for handler in line[12:].split():
                        if handler.startswith("event"):
                            devices.append((name or "?", "/dev/input/" + handler))
    except OSError as exc:
        sys.exit(f"cannot read /proc/bus/input/devices: {exc}")
    return devices


def autodetect():
    for name, path in read_devices():
        if "virtual" in name.lower() and "g502" in name.lower():
            return name, path
    sys.exit(
        "could not find the virtual G502 device - is g502d running?\n"
        "Pass a device explicitly, or run with --list to see the options."
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", nargs="?", help="evdev node, e.g. /dev/input/event18")
    parser.add_argument("--list", action="store_true", help="list input devices and exit")
    parser.add_argument("--threshold", type=float, default=DUPLICATE_MS,
                        help=f"gap below which a press counts as a duplicate (default {DUPLICATE_MS:g} ms)")
    args = parser.parse_args()

    if args.list:
        for name, path in read_devices():
            print(f"{path:22} {name}")
        return

    if args.device:
        path, name = args.device, next(
            (n for n, p in read_devices() if p == args.device), "?")
    else:
        name, path = autodetect()

    try:
        stream = open(path, "rb", buffering=0)
    except PermissionError:
        sys.exit(f"permission denied on {path} - try: sudo {' '.join(sys.argv)}")
    except OSError as exc:
        sys.exit(f"cannot open {path}: {exc}")

    print(f"watching {path}  ({name})")
    print(f"a press less than {args.threshold:g} ms after the previous one is flagged as a duplicate")
    print("click a few times, then Ctrl-C\n")
    print(f"{'time':>10}  {'event':<8} {'gap':>8}   detail")

    presses = duplicates = 0
    last_press = last_down_at = start = None
    min_gap = min_press = None

    try:
        while True:
            data = stream.read(EVENT_SIZE)
            if not data or len(data) < EVENT_SIZE:
                break
            sec, usec, etype, code, value = struct.unpack(EVENT_FORMAT, data)
            if etype != EV_KEY or code != BTN_LEFT:
                continue

            now = sec + usec / 1e6
            if start is None:
                start = now
            elapsed = now - start

            if value == 1:
                presses += 1
                gap = None if last_press is None else (now - last_press) * 1000
                if gap is not None and (min_gap is None or gap < min_gap):
                    min_gap = gap
                flag = gap is not None and gap < args.threshold
                if flag:
                    duplicates += 1
                print(f"{elapsed:10.3f}  {'down':<8} "
                      f"{'-' if gap is None else format(gap, '8.1f'):>8}   "
                      + (f"DUPLICATE  (press {presses})" if flag else f"press {presses}"))
                last_press = now
                last_down_at = now
            elif value == 0:
                held = None if last_down_at is None else (now - last_down_at) * 1000
                if held is not None and (min_press is None or held < min_press):
                    min_press = held
                print(f"{elapsed:10.3f}  {'up':<8} "
                      f"{'-' if held is None else format(held, '8.1f'):>8}   "
                      + ("" if held is None else f"held {held:.0f} ms"))
    except KeyboardInterrupt:
        pass
    finally:
        stream.close()

    print(f"\n{presses} press{'' if presses == 1 else 'es'}, "
          f"{duplicates} duplicate{'' if duplicates == 1 else 's'}")
    if min_gap is not None:
        print(f"shortest gap between presses: {min_gap:.1f} ms")
    if min_press is not None:
        print(f"shortest press:               {min_press:.1f} ms")
    if presses and not duplicates:
        print("\nevdev is clean - the kernel emitted one press per click.")
    elif duplicates:
        print("\nevdev is doubling - the duplicate is at or below the device layer.")


if __name__ == "__main__":
    main()
