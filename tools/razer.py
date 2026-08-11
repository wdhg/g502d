#!/usr/bin/env python3
"""Razer HID client: read and rebind onboard button assignments.

Razer's protocol is a 90-byte feature report, unrelated to Logitech's HID++.
The framing and CRC below come from openrazer's kernel driver (driver/razercommon.h
and .c); the button-assignment command class came from OpenSnek, which
reverse-engineered it for macOS. openrazer itself has no button remapping, and
libratbag has no Razer driver at all, so none of this is available off the shelf.

    sudo ./tools/razer.py info                 # prove we can talk to the mouse
    sudo ./tools/razer.py read 15              # read one button slot
    sudo ./tools/razer.py dump                 # scan every slot
    sudo ./tools/razer.py backup out.json      # save every slot to a file

Writes are gated behind --i-understand-this-writes-onboard-memory. Take a backup
first; these assignments live in the mouse, persist across reboots, and apply to
Windows too.
"""

import argparse
import fcntl
import glob
import json
import os
import sys
import time

# Report layout, from openrazer driver/razercommon.h
REPORT_SIZE = 90
STATUS = 0
TRANSACTION_ID = 1
REMAINING_PACKETS = 2  # be16, bytes 2..3
PROTOCOL_TYPE = 4
DATA_SIZE = 5
COMMAND_CLASS = 6
COMMAND_ID = 7
ARGUMENTS = 8  # 80 bytes, 8..87
CRC = 88

# Status byte in a response
STATUS_NAMES = {
    0x00: "new command",
    0x01: "busy",
    0x02: "success",
    0x03: "failure",
    0x04: "timeout",
    0x05: "not supported",
}

RAZER_VENDOR = "1532"

# Basilisk V3 Pro. openrazer puts 0x00AA in the "new mouse receiver" group with
# transaction id 0x1f and report index 0, i.e. USB interface 0.
DEFAULT_TRANSACTION_ID = 0x1F
CONTROL_INTERFACE = 0

# Product ids that mean the mouse is plugged in. Connection mode changes the USB
# product id on this device, so the id itself identifies the transport - more
# reliable than inferring it from a charging flag, which is also set when a
# wireless mouse is charging.
WIRED_PRODUCTS = {0x00AA}

# Button assignment, from OpenSnek Sources/OpenSnek/Bridge/BridgeClient+USB.swift
CLASS_BUTTON = 0x02
CMD_SET_BINDING = 0x0C
CMD_GET_BINDING = 0x8C
BINDING_ARG_SIZE = 10  # profile, slot, hypershift, + 7-byte function block

# Status reads. These are the same commands openrazer's kernel driver uses on
# this device, so unlike the button-assignment class they are well established.
CLASS_MISC = 0x00
CMD_GET_POLLING_RATE = 0x85
CLASS_DPI = 0x04
CMD_GET_DPI = 0x85
CLASS_POWER = 0x07
CMD_GET_BATTERY = 0x80
CMD_GET_CHARGING = 0x84

# arguments[0] of a DPI read: 0x00 reads the live value, 0x01 the stored one.
DPI_NOSTORE = 0x00
DPI_VARSTORE = 0x01

POLLING_RATES = {0x01: 1000, 0x02: 500, 0x08: 125}

# Function block classes, from OpenSnek Sources/OpenSnekCore/ButtonBindingSupport.swift
FUNCTION_CLASSES = {
    0x00: "disabled",
    0x01: "mouse button",
    0x02: "keyboard key",
    0x06: "device action (DPI cycle / sensitivity clutch)",
    0x0D: "keyboard key + turbo",
    0x0E: "mouse button + turbo",
}

# HID keyboard modifier bits for a keyboard binding's first parameter
MODIFIERS = {
    "ctrl": 0x01, "shift": 0x02, "alt": 0x04, "gui": 0x08,
    "rctrl": 0x10, "rshift": 0x20, "ralt": 0x40, "rgui": 0x80,
}

# HID keyboard usage IDs worth naming. F13-F24 are 104-115.
HID_USAGES = {name: 0x68 + i for i, name in enumerate(
    ["f13", "f14", "f15", "f16", "f17", "f18",
     "f19", "f20", "f21", "f22", "f23", "f24"])}
HID_USAGES.update({"lctrl": 0xE0, "lshift": 0xE1, "lalt": 0xE2, "lgui": 0xE3})

USAGE_NAMES = {v: k for k, v in HID_USAGES.items()}


def _ioc(direction, typ, nr, size):
    return (direction << 30) | (size << 16) | (ord(typ) << 8) | nr


def HIDIOCSFEATURE(size):
    return _ioc(3, "H", 0x06, size)


def HIDIOCGFEATURE(size):
    return _ioc(3, "H", 0x07, size)


class RazerError(Exception):
    pass


def hidraw_candidates():
    """[(interface number, node, product id)] for every connected Razer hidraw."""
    candidates = []
    for path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        uevent = os.path.join(path, "device", "uevent")
        try:
            with open(uevent) as handle:
                text = handle.read()
        except OSError:
            continue

        # HID_ID=0003:00001532:000000AA - bus, vendor, product
        product = None
        for line in text.splitlines():
            if not line.startswith("HID_ID="):
                continue
            parts = line.split("=", 1)[1].split(":")
            if len(parts) == 3 and int(parts[1], 16) == int(RAZER_VENDOR, 16):
                product = int(parts[2], 16)
            break
        if product is None:
            continue

        # .../usbN/N-M/N-M:1.X/0003:1532:00AA.0001/hidraw/hidrawN
        usb_interface = os.path.dirname(os.path.realpath(os.path.join(path, "device")))
        try:
            with open(os.path.join(usb_interface, "bInterfaceNumber")) as handle:
                number = int(handle.read().strip(), 16)
        except (OSError, ValueError):
            continue
        candidates.append((number, "/dev/" + os.path.basename(path), product))

    return sorted(candidates)


def find_control_hidraw():
    """(node, product id) for the interface that accepts control reports."""
    candidates = hidraw_candidates()
    if not candidates:
        raise RazerError("no Razer hidraw node found - is the mouse connected?")
    for number, node, product in candidates:
        if number == CONTROL_INTERFACE:
            return node, product
    # Fall back to the lowest interface rather than guessing wrong silently
    _, node, product = candidates[0]
    return node, product


class Device:
    def __init__(self, path=None, transaction_id=DEFAULT_TRANSACTION_ID, verbose=False):
        if path:
            self.path = path
            self.product = next((p for _, node, p in hidraw_candidates() if node == path), None)
        else:
            self.path, self.product = find_control_hidraw()
        self.transaction_id = transaction_id
        self.verbose = verbose
        try:
            self.fd = os.open(self.path, os.O_RDWR)
        except PermissionError as exc:
            raise RazerError(f"permission denied on {self.path} - run with sudo") from exc
        except OSError as exc:
            raise RazerError(f"cannot open {self.path}: {exc}") from exc

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @staticmethod
    def _crc(report):
        """XOR of bytes 2..87, per openrazer driver/razercommon.c."""
        value = 0
        for byte in report[2:88]:
            value ^= byte
        return value

    def _build(self, command_class, command_id, args):
        report = bytearray(REPORT_SIZE)
        report[STATUS] = 0x00
        report[TRANSACTION_ID] = self.transaction_id
        report[REMAINING_PACKETS] = 0x00
        report[REMAINING_PACKETS + 1] = 0x00
        report[PROTOCOL_TYPE] = 0x00
        report[DATA_SIZE] = len(args)
        report[COMMAND_CLASS] = command_class
        report[COMMAND_ID] = command_id
        report[ARGUMENTS:ARGUMENTS + len(args)] = bytes(args)
        report[CRC] = self._crc(report)
        return report

    def request(self, command_class, command_id, args=b"", attempts=12, delay=0.04):
        """Send a command and return the device's response report."""
        report = self._build(command_class, command_id, args)
        if self.verbose:
            print(f"  -> class=0x{command_class:02x} id=0x{command_id:02x} "
                  f"args={bytes(args).hex(' ')}", file=sys.stderr)

        # hidraw wants the report number in byte 0; for unnumbered reports the
        # kernel offsets the payload by one in both directions.
        out = bytes([0x00]) + bytes(report)
        try:
            fcntl.ioctl(self.fd, HIDIOCSFEATURE(len(out)), out)
        except OSError as exc:
            raise RazerError(f"HIDIOCSFEATURE failed: {exc}")

        last_status = None
        for _ in range(attempts):
            time.sleep(delay)
            buf = bytearray(REPORT_SIZE + 1)
            try:
                fcntl.ioctl(self.fd, HIDIOCGFEATURE(len(buf)), buf, True)
            except OSError as exc:
                raise RazerError(f"HIDIOCGFEATURE failed: {exc}")
            response = bytes(buf[1:])

            status = response[STATUS]
            last_status = status
            if self.verbose:
                print(f"  <- status=0x{status:02x} ({STATUS_NAMES.get(status, '?')}) "
                      f"args={response[ARGUMENTS:ARGUMENTS + 16].hex(' ')}", file=sys.stderr)
            if status == 0x01:
                continue  # busy, retry
            if status == 0x02:
                return response
            raise RazerError(
                f"device returned status 0x{status:02x} "
                f"({STATUS_NAMES.get(status, 'unknown')}) for "
                f"class=0x{command_class:02x} id=0x{command_id:02x}")

        raise RazerError(f"no successful response (last status {last_status})")

    # -- specific commands ---------------------------------------------

    def firmware_version(self):
        response = self.request(0x00, 0x81, bytes(2))
        return response[ARGUMENTS], response[ARGUMENTS + 1]

    def serial(self):
        response = self.request(0x00, 0x82, bytes(22))
        raw = response[ARGUMENTS:ARGUMENTS + 22]
        return raw.split(b"\x00")[0].decode("ascii", "replace")

    def is_wired(self):
        """True when the mouse is plugged in, so battery is not worth reporting."""
        return self.product in WIRED_PRODUCTS

    def battery_percent(self):
        """Charge as a percentage, or None if the device does not report one."""
        response = self.request(CLASS_POWER, CMD_GET_BATTERY, bytes(2))
        raw = response[ARGUMENTS + 1]
        # A wired-only mouse answers 0; treat that as "no battery" rather than
        # reporting a flat one.
        return round(raw * 100 / 255) if raw else None

    def is_charging(self):
        response = self.request(CLASS_POWER, CMD_GET_CHARGING, bytes(2))
        return bool(response[ARGUMENTS + 1])

    def dpi(self):
        """(x, y) DPI. Falls back to the stored value if the live read fails."""
        for store in (DPI_NOSTORE, DPI_VARSTORE):
            try:
                response = self.request(CLASS_DPI, CMD_GET_DPI, bytes([store]) + bytes(6))
            except RazerError:
                continue
            args = response[ARGUMENTS:ARGUMENTS + 5]
            return (args[1] << 8) | args[2], (args[3] << 8) | args[4]
        raise RazerError("could not read DPI")

    def polling_rate(self):
        """Reports in Hz. Returns None for a rate this command cannot express."""
        response = self.request(CLASS_MISC, CMD_GET_POLLING_RATE, bytes(1))
        return POLLING_RATES.get(response[ARGUMENTS])

    def read_binding(self, slot, profile=1, hypershift=0):
        args = bytes([profile, slot, hypershift]) + bytes(7)
        response = self.request(CLASS_BUTTON, CMD_GET_BINDING, args)
        return response[ARGUMENTS + 3:ARGUMENTS + 10]

    def write_binding(self, slot, block, profile=1, hypershift=0):
        if len(block) != 7:
            raise RazerError("function block must be 7 bytes")
        args = bytes([profile, slot, hypershift]) + bytes(block)
        self.request(CLASS_BUTTON, CMD_SET_BINDING, args)


def describe_block(block):
    """Human-readable summary of a 7-byte function block."""
    kind = block[0]
    name = FUNCTION_CLASSES.get(kind, f"unknown (0x{kind:02x})")
    length = block[1]
    params = block[2:2 + length] if length <= 5 else block[2:]

    if kind == 0x02 and length >= 2:
        mods = [n for n, bit in MODIFIERS.items() if params[0] & bit]
        key = USAGE_NAMES.get(params[1], f"usage 0x{params[1]:02x}")
        detail = "+".join(mods + [key]) if mods else key
        return f"{name}: {detail}"
    if kind == 0x01 and length >= 1:
        return f"{name}: button {params[0]}"
    if kind == 0x00:
        return name
    if kind == 0x06 and len(block) >= 7:
        # Declared length is 1, but the trailing four bytes carry the clutch
        # target as two big-endian u16s - 01 90 01 90 is 400 DPI on both axes.
        dpi_x = (block[3] << 8) | block[4]
        dpi_y = (block[5] << 8) | block[6]
        if dpi_x or dpi_y:
            return f"{name}, action {params.hex()} -> {dpi_x}/{dpi_y} DPI"
    return f"{name}, params {params.hex(' ')}"


def keyboard_block(usage, modifiers=0):
    """The 7-byte block binding a slot to a keyboard key."""
    return bytes([0x02, 0x02, modifiers, usage, 0x00, 0x00, 0x00])


def parse_key(text):
    key = text.lower()
    if key in HID_USAGES:
        return HID_USAGES[key]
    try:
        return int(text, 0)
    except ValueError:
        raise SystemExit(f"unknown key {text!r} - use a name ({', '.join(sorted(HID_USAGES))}) "
                         f"or a HID usage like 0x73")


def cmd_info(dev, _args):
    print(f"control node: {dev.path}")
    print(f"transaction id: 0x{dev.transaction_id:02x}")
    major, minor = dev.firmware_version()
    print(f"firmware: v{major}.{minor}")
    print(f"serial: {dev.serial()}")


def cmd_status(dev, args):
    """Everything the mouse will tell us about itself."""
    stats = {"wired": dev.is_wired()}
    for name, getter in (("firmware", lambda: "v%d.%d" % dev.firmware_version()),
                         ("serial", dev.serial),
                         ("battery_percent", dev.battery_percent),
                         ("charging", dev.is_charging),
                         ("dpi", dev.dpi),
                         ("polling_rate_hz", dev.polling_rate)):
        try:
            stats[name] = getter()
        except RazerError as exc:
            stats[name] = None
            if args.verbose:
                print(f"  {name}: {exc}", file=sys.stderr)

    if args.json:
        print(json.dumps(stats))
        return

    print(f"device:   {dev.path}")
    print(f"firmware: {stats['firmware'] or 'unknown'}")
    print(f"serial:   {stats['serial'] or 'unknown'}")
    if dev.is_wired():
        print("power:    wired")
    elif stats["battery_percent"] is None:
        print("battery:  not reported")
    else:
        print(f"battery:  {stats['battery_percent']}%"
              f"{' (charging)' if stats['charging'] else ''}")
    if stats["dpi"]:
        x, y = stats["dpi"]
        print(f"dpi:      {x}" + (f" x {y}" if x != y else ""))
    if stats["polling_rate_hz"]:
        print(f"polling:  {stats['polling_rate_hz']} Hz")


def cmd_read(dev, args):
    block = dev.read_binding(args.slot, profile=args.profile)
    print(f"profile {args.profile} slot {args.slot} (0x{args.slot:02x}): {block.hex(' ')}")
    print(f"  {describe_block(block)}")


def cmd_dump(dev, args):
    print(f"scanning slots {args.first}-{args.last} of profile {args.profile}\n")
    for slot in range(args.first, args.last + 1):
        try:
            block = dev.read_binding(slot, profile=args.profile)
        except RazerError as exc:
            print(f"  slot {slot:3} (0x{slot:02x}): -- {exc}")
            continue
        print(f"  slot {slot:3} (0x{slot:02x}): {block.hex(' ')}  {describe_block(block)}")


def cmd_backup(dev, args):
    saved = {}
    for slot in range(args.first, args.last + 1):
        try:
            saved[slot] = dev.read_binding(slot, profile=args.profile).hex()
        except RazerError:
            continue
    if not saved:
        raise SystemExit("nothing could be read - refusing to write an empty backup")
    payload = {"profile": args.profile, "device": dev.path, "bindings": saved}
    with open(args.path, "w") as handle:
        json.dump(payload, handle, indent=2)
    print(f"saved {len(saved)} slots to {args.path}")


def cmd_set_key(dev, args):
    usage = parse_key(args.key)
    modifiers = 0
    for name in args.modifier or []:
        if name.lower() not in MODIFIERS:
            raise SystemExit(f"unknown modifier {name!r} - one of {', '.join(MODIFIERS)}")
        modifiers |= MODIFIERS[name.lower()]

    before = dev.read_binding(args.slot, profile=args.profile)
    block = keyboard_block(usage, modifiers)
    print(f"profile {args.profile} slot {args.slot} (0x{args.slot:02x})")
    print(f"  before: {before.hex(' ')}  {describe_block(before)}")
    print(f"  after:  {block.hex(' ')}  {describe_block(block)}")

    if not args.commit:
        print("\ndry run. re-run with --i-understand-this-writes-onboard-memory to apply.")
        return

    dev.write_binding(args.slot, block, profile=args.profile)
    readback = dev.read_binding(args.slot, profile=args.profile)
    print(f"  readback: {readback.hex(' ')}  {describe_block(readback)}")
    if readback != block:
        raise SystemExit("readback does not match what was written")
    print("written and verified.")


def cmd_restore(dev, args):
    with open(args.path) as handle:
        payload = json.load(handle)
    bindings = payload["bindings"]
    profile = payload.get("profile", 1)

    print(f"restoring {len(bindings)} slots to profile {profile}")
    for slot, hexblock in sorted(bindings.items(), key=lambda kv: int(kv[0])):
        block = bytes.fromhex(hexblock)
        current = dev.read_binding(int(slot), profile=profile)
        if current == block:
            continue
        print(f"  slot {slot}: {current.hex(' ')} -> {block.hex(' ')}")
        if args.commit:
            dev.write_binding(int(slot), block, profile=profile)
    if not args.commit:
        print("\ndry run. re-run with --i-understand-this-writes-onboard-memory to apply.")


def main():
    # Global options are defined twice: once on the top level with real
    # defaults, and once on a parent shared by every subcommand with
    # SUPPRESS defaults. That way they work either side of the subcommand
    # name, and an unused one on the right does not clobber a value given
    # on the left. Getting this wrong makes the write flag look unrecognised.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--device", default=argparse.SUPPRESS)
    common.add_argument("--transaction-id", type=lambda s: int(s, 0),
                        default=argparse.SUPPRESS)
    common.add_argument("--profile", type=lambda s: int(s, 0),
                        default=argparse.SUPPRESS)
    common.add_argument("-v", "--verbose", action="store_true",
                        default=argparse.SUPPRESS)
    common.add_argument("--i-understand-this-writes-onboard-memory", dest="commit",
                        action="store_true", default=argparse.SUPPRESS)

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", help="hidraw node (default: autodetect interface 0)")
    parser.add_argument("--transaction-id", type=lambda s: int(s, 0),
                        default=DEFAULT_TRANSACTION_ID,
                        help="protocol transaction id (default 0x1f, Basilisk V3 Pro)")
    parser.add_argument("--profile", type=lambda s: int(s, 0), default=1,
                        help="onboard profile (default 1; 0 is the volatile layer)")
    parser.add_argument("-v", "--verbose", action="store_true", help="show raw reports")
    parser.add_argument("--i-understand-this-writes-onboard-memory", dest="commit",
                        action="store_true", help="actually write; otherwise dry run")

    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("info", parents=[common],
                   help="firmware and serial - proves the transport works")

    p = sub.add_parser("status", parents=[common],
                       help="battery, DPI, polling rate and firmware")
    p.add_argument("--json", action="store_true", help="machine-readable output")

    p = sub.add_parser("read", parents=[common], help="read one button slot")
    p.add_argument("slot", type=lambda s: int(s, 0))

    p = sub.add_parser("dump", parents=[common], help="scan a range of button slots")
    p.add_argument("--first", type=lambda s: int(s, 0), default=1)
    p.add_argument("--last", type=lambda s: int(s, 0), default=20)

    p = sub.add_parser("backup", parents=[common],
                       help="save every readable slot to a file")
    p.add_argument("path")
    p.add_argument("--first", type=lambda s: int(s, 0), default=1)
    p.add_argument("--last", type=lambda s: int(s, 0), default=100)

    p = sub.add_parser("set-key", parents=[common], help="bind a slot to a keyboard key")
    p.add_argument("slot", type=lambda s: int(s, 0))
    p.add_argument("key", help="name (f13, lalt, ...) or HID usage (0x73)")
    p.add_argument("--modifier", action="append", help="ctrl/shift/alt/gui, repeatable")

    p = sub.add_parser("restore", parents=[common],
                       help="restore slots from a backup file")
    p.add_argument("path")

    args = parser.parse_args()
    handlers = {"info": cmd_info, "status": cmd_status, "read": cmd_read, "dump": cmd_dump,
                "backup": cmd_backup, "set-key": cmd_set_key, "restore": cmd_restore}

    try:
        with Device(args.device, args.transaction_id, args.verbose) as dev:
            handlers[args.command](dev, args)
    except RazerError as exc:
        sys.exit(f"error: {exc}")


if __name__ == "__main__":
    main()
