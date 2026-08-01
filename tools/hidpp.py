#!/usr/bin/env python3
"""Minimal HID++ 2.0 client for the wired G502 (046d:c332) over /dev/hidraw1.

The G502 exposes Logitech's HID++ protocol on interface 1 as two vendor reports:
  0x10 "short" - 7 bytes  (4 header + 3 params)
  0x11 "long"  - 20 bytes (4 header + 16 params)

Header layout: [report_id, device_index, feature_index, function<<4 | software_id]

For a corded device there is no receiver, so device_index is 0xff.

Usage:
    ./hidpp.py features                 # enumerate every feature the device implements
    ./hidpp.py controls                 # dump 0x1b04 reprogrammable controls + divert state
    ./hidpp.py profiles                 # dump 0x8100 onboard profile info
    ./hidpp.py raw 10 ff 00 00 1b 04 00 # send an arbitrary report
"""

import os
import select
import sys
import time

HIDRAW = "/dev/hidraw1"
DEV_INDEX = 0xFF
SW_ID = 0x0A  # any nonzero nibble; echoed back so we can match our own replies

SHORT, LONG = 0x10, 0x11
SHORT_LEN, LONG_LEN = 7, 20

ERR_HIDPP20 = 0xFF  # feature_index in an error reply
ERR_HIDPP10 = 0x8F

HIDPP20_ERRORS = {
    0x00: "NoError",
    0x01: "Unknown",
    0x02: "InvalidArgument",
    0x03: "OutOfRange",
    0x04: "HWError",
    0x05: "LogitechInternal",
    0x06: "InvalidFeatureIndex",
    0x07: "InvalidFunctionID",
    0x08: "Busy",
    0x09: "Unsupported",
}

# Feature IDs we care about, plus a wider table for pretty-printing enumeration.
FEATURES = {
    0x0000: "ROOT",
    0x0001: "FEATURE_SET",
    0x0002: "FEATURE_INFO",
    0x0003: "DEVICE_FW_VERSION",
    0x0005: "DEVICE_NAME",
    0x0020: "CONFIG_CHANGE",
    0x00C2: "DFUCONTROL_UNSIGNED",
    0x1000: "BATTERY_LEVEL_STATUS",
    0x1001: "BATTERY_VOLTAGE",
    0x1300: "LED_CONTROL",
    0x1802: "DEVICE_RESET",
    0x1B04: "SPECIAL_KEYS_MSE_BUTTONS",
    0x1DF3: "EQUAD_DJ_DEVICE_PAIRING",
    0x1E00: "ENABLE_HIDDEN_FEATURES",
    0x1F03: "TEMPERATURE_MEASURE",
    0x2201: "ADJUSTABLE_DPI",
    0x2205: "POINTER_MOTION_SCALING",
    0x8010: "GKEY",
    0x8020: "MKEYS",
    0x8030: "MR",
    0x8060: "REPORT_RATE",
    0x8070: "COLOR_LED_EFFECTS",
    0x8071: "RGB_EFFECTS",
    0x8100: "ONBOARD_PROFILES",
    0x8110: "MOUSE_BUTTON_SPY",
}

# 0x1b04 task IDs (control "meaning"), abridged to what a G502 can carry.
TASKS = {
    0x0001: "Volume Up",
    0x0002: "Volume Down",
    0x0032: "Left Click",
    0x0033: "Right Click",
    0x0034: "Middle Mouse Button",
    0x0035: "Back",
    0x0036: "Forward",
    0x0037: "Mouse Scroll Left",
    0x0038: "Mouse Scroll Right",
    0x0039: "DPI Switch / Cycle",
    0x003A: "Gesture Button",
    0x003B: "DPI Shift (sniper)",
    0x004A: "Smart Shift",
    0x0050: "Switch Profile",
    0x006D: "DPI Up",
    0x006E: "DPI Down",
    0x00B4: "Gesture Button Navigation",
}

CID_NAMES = {
    0x0050: "Left Click",
    0x0051: "Right Click",
    0x0052: "Middle Mouse Button",
    0x0053: "Back (thumb, rear)",
    0x0056: "Forward (thumb, front)",
    0x0057: "DPI Shift / sniper (thumb paddle)",
    0x005B: "DPI Up",
    0x005C: "DPI Down",
    0x005D: "DPI Cycle",
    0x0056: "Forward",
    0x00E0: "Scroll Left (tilt)",
    0x00E1: "Scroll Right (tilt)",
    0x00E2: "Profile Cycle",
}


class HidppError(Exception):
    pass


class Device:
    def __init__(self, path=HIDRAW):
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self._feature_cache = {}

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _drain(self):
        """Discard anything already queued so stale notifications can't be mistaken
        for our reply."""
        while select.select([self.fd], [], [], 0)[0]:
            try:
                os.read(self.fd, 64)
            except (BlockingIOError, OSError):
                break

    def request(self, feature_index, function, params=b"", long=False, timeout=1.0):
        rid = LONG if long else SHORT
        size = LONG_LEN if long else SHORT_LEN
        payload = bytes(params).ljust(size - 4, b"\x00")
        if len(payload) > size - 4:
            raise ValueError("params too long for this report size")
        msg = bytes([rid, DEV_INDEX, feature_index, (function << 4) | SW_ID]) + payload

        self._drain()
        os.write(self.fd, msg)

        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise HidppError(
                    f"timeout waiting for reply to feature {feature_index:#04x} fn {function}"
                )
            if not select.select([self.fd], [], [], remaining)[0]:
                continue
            try:
                buf = os.read(self.fd, 64)
            except BlockingIOError:
                continue
            if len(buf) < 4 or buf[0] not in (SHORT, LONG):
                continue  # not a HID++ report
            if buf[1] != DEV_INDEX:
                continue

            # HID++ 2.0 error: [id, dev, 0xff, feature_index, function|swid, code]
            if buf[2] == ERR_HIDPP20 and len(buf) >= 6:
                if buf[3] == feature_index and buf[4] == (function << 4) | SW_ID:
                    code = buf[5]
                    raise HidppError(
                        f"HID++2.0 error {code:#04x} "
                        f"({HIDPP20_ERRORS.get(code, 'reserved')}) "
                        f"on feature {feature_index:#04x} fn {function}"
                    )
                continue
            # HID++ 1.0 error: [id, dev, 0x8f, sub_id, address, code]
            if buf[2] == ERR_HIDPP10 and len(buf) >= 6:
                raise HidppError(f"HID++1.0 error {buf[5]:#04x}")

            # Normal reply must echo feature index and function|swid.
            if buf[2] == feature_index and buf[3] == (function << 4) | SW_ID:
                return buf[4:]
            # else: an unsolicited notification, keep waiting

    # -- feature discovery -------------------------------------------------

    def feature_index(self, feature_id):
        """Root.getFeature - map a 16-bit feature ID to its per-device index."""
        if feature_id in self._feature_cache:
            return self._feature_cache[feature_id]
        r = self.request(0x00, 0, bytes([feature_id >> 8, feature_id & 0xFF, 0]))
        index, ftype, version = r[0], r[1], r[2]
        if index == 0:
            raise HidppError(f"feature {feature_id:#06x} not supported")
        self._feature_cache[feature_id] = index
        return index

    def feature_details(self, feature_id):
        r = self.request(0x00, 0, bytes([feature_id >> 8, feature_id & 0xFF, 0]))
        return r[0], r[1], r[2]  # index, type, version

    def enumerate_features(self):
        fs_index = self.feature_index(0x0001)
        count = self.request(fs_index, 0)[0]
        out = []
        for i in range(count + 1):  # index 0 is ROOT itself
            r = self.request(fs_index, 1, bytes([i]))
            fid = (r[0] << 8) | r[1]
            ftype, version = r[2], r[3] if len(r) > 3 else 0
            out.append((i, fid, ftype, version))
        return out


def decode_feature_type(t):
    bits = []
    if t & 0x80:
        bits.append("obsolete")
    if t & 0x40:
        bits.append("hidden")
    if t & 0x20:
        bits.append("engineering")
    if t & 0x10:
        bits.append("manufacturing-deactivatable")
    if t & 0x08:
        bits.append("compliance-deactivatable")
    return ",".join(bits) or "-"


def cmd_features(dev):
    print(f"{'idx':>4}  {'feature':>8}  {'name':<28} {'ver':>3}  flags")
    for index, fid, ftype, version in dev.enumerate_features():
        name = FEATURES.get(fid, "?")
        print(
            f"{index:>4}  {fid:#06x}  {name:<28} {version:>3}  {decode_feature_type(ftype)}"
        )


def cmd_controls(dev):
    idx = dev.feature_index(0x1B04)
    _, _, version = dev.feature_details(0x1B04)
    count = dev.request(idx, 0)[0]
    print(f"0x1b04 SPECIAL_KEYS_MSE_BUTTONS v{version}, feature index {idx}, "
          f"{count} controls\n")
    for i in range(count):
        r = dev.request(idx, 1, bytes([i]), long=True)
        cid = (r[0] << 8) | r[1]
        tid = (r[2] << 8) | r[3]
        flags, pos, group, gmask = r[4], r[5], r[6], r[7]
        addflags = r[8] if len(r) > 8 else 0

        caps = []
        if flags & 0x01:
            caps.append("mouse")
        if flags & 0x02:
            caps.append("fkey")
        if flags & 0x04:
            caps.append("hotkey")
        if flags & 0x08:
            caps.append("fn-toggle")
        if flags & 0x10:
            caps.append("reprogrammable")
        if flags & 0x20:
            caps.append("DIVERTABLE")
        if flags & 0x40:
            caps.append("persist-divert")
        if flags & 0x80:
            caps.append("virtual")
        if addflags & 0x01:
            caps.append("raw-xy")
        if addflags & 0x02:
            caps.append("force-raw-xy")
        if addflags & 0x04:
            caps.append("analytics-key-events")

        # current reporting state
        try:
            s = dev.request(idx, 2, bytes([r[0], r[1]]), long=True)
            sflags = s[2]
            state = []
            if sflags & 0x01:
                state.append("diverted")
            if sflags & 0x04:
                state.append("persist-diverted")
            if sflags & 0x10:
                state.append("raw-xy-diverted")
            remap = (s[3] << 8) | s[4] if len(s) > 4 else 0
            state_s = ",".join(state) or "normal"
            if remap:
                state_s += f" remap->{remap:#06x}"
        except HidppError as exc:
            state_s = f"<{exc}>"

        print(
            f"  [{i:>2}] cid={cid:#06x} {CID_NAMES.get(cid, ''):<34} "
            f"task={tid:#06x} {TASKS.get(tid, '?'):<22} "
            f"pos={pos} grp={group} mask={gmask:#04x}\n"
            f"       caps: {','.join(caps) or '-'}\n"
            f"       now:  {state_s}"
        )


def cmd_profiles(dev):
    idx = dev.feature_index(0x8100)
    _, _, version = dev.feature_details(0x8100)
    r = dev.request(idx, 0, long=True)
    names = [
        "memoryModelID", "profileFormatID", "macroFormatID", "profileCount",
        "profileCountOOB", "buttonCount", "sectorCount", "sectorSizeHi",
        "sectorSizeLo", "mechanicalLayout", "variousInfo",
    ]
    print(f"0x8100 ONBOARD_PROFILES v{version}, feature index {idx}")
    for n, v in zip(names, r):
        print(f"  {n:<18} {v:#04x} ({v})")
    mode = dev.request(idx, 2)[0]
    cur = dev.request(idx, 4, long=True)
    print(f"  mode               {mode:#04x} "
          f"({'onboard' if mode == 1 else 'host' if mode == 2 else '?'})")
    print(f"  current profile    {cur[0]:#04x} {cur[1]:#04x}")


def read_sector(dev, sector, size=256):
    """0x8100 fn5 memoryRead(sectorId:2, subAddress:2) -> 16 bytes per call."""
    idx = dev.feature_index(0x8100)
    out = bytearray()
    for off in range(0, size, 16):
        r = dev.request(
            idx, 5,
            bytes([sector >> 8, sector & 0xFF, off >> 8, off & 0xFF]),
            long=True,
        )
        out += r[:16]
    return bytes(out)


def hexdump(data, base=0):
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        ascii_ = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {base + i:04x}  {hexs:<47}  {ascii_}")
    return "\n".join(lines)


def cmd_dump(dev, argv):
    sectors = [int(x, 0) for x in argv] if argv else [0x0000, 0x0001, 0x0002, 0x0003]
    for s in sectors:
        print(f"=== sector {s:#06x} ===")
        try:
            print(hexdump(read_sector(dev, s)))
        except HidppError as exc:
            print(f"  <{exc}>")


def cmd_raw(dev, argv):
    data = bytes(int(x, 16) for x in argv)
    if len(data) < 4:
        sys.exit("raw needs at least 4 bytes")
    size = LONG_LEN if data[0] == LONG else SHORT_LEN
    dev._drain()
    os.write(dev.fd, data.ljust(size, b"\x00"))
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        if select.select([dev.fd], [], [], deadline - time.monotonic())[0]:
            buf = os.read(dev.fd, 64)
            print(" ".join(f"{b:02x}" for b in buf))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    with Device() as dev:
        try:
            if cmd == "features":
                cmd_features(dev)
            elif cmd == "controls":
                cmd_controls(dev)
            elif cmd == "profiles":
                cmd_profiles(dev)
            elif cmd == "dump":
                cmd_dump(dev, sys.argv[2:])
            elif cmd == "raw":
                cmd_raw(dev, sys.argv[2:])
            else:
                sys.exit(__doc__)
        except HidppError as exc:
            sys.exit(f"error: {exc}")


if __name__ == "__main__":
    main()
