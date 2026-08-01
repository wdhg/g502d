#!/usr/bin/env python3
"""Decode (and optionally rewrite) the G502's onboard profiles.

Layout was recovered by dumping this device's own flash; it matches libratbag's
"profile format 2 / memory model 1". A sector is 256 bytes:

    0x00  u8   report_rate      milliseconds per report (1 = 1000Hz, 2 = 500Hz, ...)
    0x01  u8   default_dpi      index into dpi[]
    0x02  u8   shift_dpi        index into dpi[] used while DPI-shift is held
    0x03  u16  dpi[5]           little-endian, 0 = slot unused
    0x0d  u8   color[3]         profile colour (RGB)
    0x10  u8   power_mode
    0x11  u8   angle_snapping
    0x12       reserved
    0x20  bind buttons[16]      4 bytes each; this device uses the first 11
    0x60  bind alt_buttons[16]  G-shift bank, unused here (all 0xff)
    0xa0  u16  name[24]         UTF-16LE, NUL padded, 48 bytes, runs to 0xcf
    0xd0       logo LED block (11 bytes)
    0xdb       side LED block (11 bytes)
    0xe6       alternate logo LED block, 0xf1 alternate side LED block
    0xfc  u8   custom animation index
    0xfe  u16  crc              CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff) over 0x00..0xfd

A button binding is 4 bytes:

    80 01 <u16 mask>        HID mouse button, mask is a bitfield (bit0 = button 1)
    80 02 <mods> <keycode>  HID keyboard: modifier bitmask + HID usage id
    80 03 <u16 usage>       HID consumer control
    90 <action> ff ff       device-internal special action, never sent to the host
    ff ff ff ff             disabled

Sector 0 is the profile directory: 4-byte entries of
    <u16 sector> <u8 enabled> <u8 reserved>, terminated by ffff.

Usage:
    ./profile.py show                 # decode every sector read live from the mouse
    ./profile.py show --file b.bin    # decode a backup made by profile_backup.py
    ./profile.py plan 1 6 mouse:6     # show the exact byte diff for a rebind, write nothing
    ./profile.py apply 1 6 mouse:6 --i-understand-this-writes-flash
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from hidpp import Device, HidppError, hexdump, read_sector

SECTOR_SIZE = 256
BUTTONS_OFFSET = 0x20
ALT_BUTTONS_OFFSET = 0x60
NAME_OFFSET = 0xA0
NAME_SIZE = 48  # 24 UTF-16LE units, running to 0xcf; the LED blocks start at 0xd0
BINDING_SIZE = 4
CRC_OFFSET = 0xFE

TYPE_HID = 0x80
TYPE_SPECIAL = 0x90
TYPE_DISABLED = 0xFF

HID_SUB_MOUSE = 0x01
HID_SUB_KEYBOARD = 0x02
HID_SUB_CONSUMER = 0x03

# Device-internal actions. Values confirmed against this mouse's factory profiles:
# profile 1 uses 0x07 on the thumb paddle (DPI shift) and 0x01/0x02 on wheel tilt.
SPECIAL_ACTIONS = {
    0x00: "none",
    0x01: "wheel left",
    0x02: "wheel right",
    0x03: "DPI up",
    0x04: "DPI down",
    0x05: "DPI cycle up",
    0x06: "DPI cycle down",
    0x07: "DPI shift (sniper) - handled on-device, NOT sent to host",
    0x08: "DPI default",
    0x0A: "profile cycle up",
    0x0B: "profile cycle down",
}

# USB HID keyboard modifier bitmask (Usage Page 0x07, bits per HID spec).
MODIFIERS = {
    0x01: "LeftCtrl",
    0x02: "LeftShift",
    0x04: "LeftAlt",
    0x08: "LeftGUI",
    0x10: "RightCtrl",
    0x20: "RightShift",
    0x40: "RightAlt",
    0x80: "RightGUI",
}

# HID keyboard usage ids we might want to name. Enough to read the factory profiles.
HID_KEYS = {0x00: "(none)"}
for _i, _c in enumerate("abcdefghijklmnopqrstuvwxyz"):
    HID_KEYS[0x04 + _i] = _c
for _i, _c in enumerate("1234567890"):
    HID_KEYS[0x1E + _i] = _c
HID_KEYS.update({
    0x28: "Enter", 0x29: "Esc", 0x2A: "Backspace", 0x2B: "Tab", 0x2C: "Space",
    0x3A: "F1", 0x3B: "F2", 0x3C: "F3", 0x3D: "F4", 0x3E: "F5", 0x3F: "F6",
    0x40: "F7", 0x41: "F8", 0x42: "F9", 0x43: "F10", 0x44: "F11", 0x45: "F12",
})

# Physical button order for a G502, derived from the factory profile 1 bindings
# (buttons 1-5 are real HID buttons; 6-11 are on-device actions).
BUTTON_NAMES = [
    "left click", "right click", "middle click",
    "thumb rear (back)", "thumb front (forward)", "thumb paddle (sniper)",
    "behind-wheel forward", "behind-wheel back", "profile switch (underside)",
    "wheel tilt right", "wheel tilt left",
]

# Kernel maps HID mouse button N to BTN_MOUSE + (N-1); see drivers/hid/hid-input.c.
EVDEV_FOR_MOUSE_BUTTON = {
    1: "BTN_LEFT", 2: "BTN_RIGHT", 3: "BTN_MIDDLE", 4: "BTN_SIDE",
    5: "BTN_EXTRA", 6: "BTN_FORWARD", 7: "BTN_BACK", 8: "BTN_TASK",
}


def crc_ccitt_false(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def decode_binding(b):
    if b == b"\xff\xff\xff\xff":
        return "disabled"
    t = b[0]
    if t == TYPE_HID:
        sub = b[1]
        if sub == HID_SUB_MOUSE:
            # Big-endian, unlike the DPI slots above: 80 01 00 10 is button 5 (forward).
            mask = (b[2] << 8) | b[3]
            btns = [i + 1 for i in range(16) if mask & (1 << i)]
            named = ", ".join(
                f"button {n} -> {EVDEV_FOR_MOUSE_BUTTON.get(n, f'BTN_MOUSE+{n - 1}')}"
                for n in btns
            )
            return f"HID mouse: {named or 'none'} (mask {mask:#06x})"
        if sub == HID_SUB_KEYBOARD:
            mods = [n for bit, n in MODIFIERS.items() if b[2] & bit]
            key = HID_KEYS.get(b[3], f"usage {b[3]:#04x}")
            combo = "+".join(mods + ([key] if b[3] else []))
            return f"HID keyboard: {combo or 'none'} (mods {b[2]:#04x}, usage {b[3]:#04x})"
        if sub == HID_SUB_CONSUMER:
            usage = (b[2] << 8) | b[3]
            return f"HID consumer control: usage {usage:#06x}"
        return f"HID unknown subtype {sub:#04x}"
    if t == TYPE_SPECIAL:
        return f"special {b[1]:#04x}: {SPECIAL_ACTIONS.get(b[1], 'unknown')}"
    return f"unknown type {t:#04x}"


def encode_binding(spec):
    """Parse a CLI binding spec into 4 bytes.

    mouse:N              HID mouse button N (1..16)
    key:MOD+MOD+KEY      HID keyboard, e.g. key:LeftAlt or key:LeftCtrl+c
    special:0xNN         device-internal action
    disabled
    """
    spec = spec.strip()
    if spec == "disabled":
        return b"\xff\xff\xff\xff"
    kind, _, rest = spec.partition(":")
    if kind == "mouse":
        n = int(rest, 0)
        if not 1 <= n <= 16:
            raise ValueError("mouse button must be 1..16")
        mask = 1 << (n - 1)
        return bytes([TYPE_HID, HID_SUB_MOUSE, mask >> 8, mask & 0xFF])
    if kind == "key":
        mods, keycode = 0, 0
        rev_mod = {v.lower(): k for k, v in MODIFIERS.items()}
        rev_key = {v.lower(): k for k, v in HID_KEYS.items() if k}
        for part in rest.split("+"):
            p = part.strip().lower()
            if p in rev_mod:
                mods |= rev_mod[p]
            elif p in rev_key:
                keycode = rev_key[p]
            elif p.startswith("0x"):
                keycode = int(p, 16)
            else:
                raise ValueError(f"unknown key token {part!r}")
        return bytes([TYPE_HID, HID_SUB_KEYBOARD, mods, keycode])
    if kind == "special":
        return bytes([TYPE_SPECIAL, int(rest, 0), 0xFF, 0xFF])
    raise ValueError(f"unknown binding spec {spec!r}")


def decode_directory(sec):
    entries = []
    for off in range(0, 64, 4):
        sector = (sec[off] << 8) | sec[off + 1]
        if sector == 0xFFFF:
            break
        entries.append((sector, sec[off + 2], sec[off + 3]))
    return entries


def decode_profile(sec):
    out = []
    out.append(f"  report rate      {sec[0]} ms"
               f"{f' ({1000 // sec[0]} Hz)' if sec[0] else ''}")
    out.append(f"  default DPI idx  {sec[1]}")
    out.append(f"  shift DPI idx    {sec[2]}")
    dpis = [sec[3 + i * 2] | (sec[4 + i * 2] << 8) for i in range(5)]
    out.append(f"  DPI slots        {', '.join(str(d) if d else '-' for d in dpis)}")
    out.append(f"  colour           #{sec[0x0D]:02x}{sec[0x0E]:02x}{sec[0x0F]:02x}")

    raw_name = sec[NAME_OFFSET:NAME_OFFSET + NAME_SIZE]
    name = raw_name.decode("utf-16-le", errors="replace").split("\x00")[0]
    out.append(f"  name             {name!r}")

    out.append("  buttons:")
    for i in range(11):
        off = BUTTONS_OFFSET + i * BINDING_SIZE
        b = sec[off:off + BINDING_SIZE]
        label = BUTTON_NAMES[i] if i < len(BUTTON_NAMES) else f"button {i + 1}"
        out.append(f"    [{i + 1:2d}] @{off:#04x} {b.hex(' ')}  {label:<28} {decode_binding(b)}")

    stored = (sec[CRC_OFFSET] << 8) | sec[CRC_OFFSET + 1]
    calc = crc_ccitt_false(sec[:CRC_OFFSET])
    out.append(f"  crc              stored {stored:#06x} calc {calc:#06x} "
               f"{'OK' if stored == calc else 'MISMATCH'}")
    return "\n".join(out)


def load_sectors(path=None):
    if path:
        with open(path, "rb") as fh:
            blob = fh.read()
        present = int.from_bytes(blob[-2:], "big")
        return {
            s: blob[s * SECTOR_SIZE:(s + 1) * SECTOR_SIZE]
            for s in range(16) if present & (1 << s)
        }
    sectors = {}
    with Device() as dev:
        for s in range(16):
            try:
                sectors[s] = read_sector(dev, s, SECTOR_SIZE)
            except HidppError:
                pass
    return sectors


def cmd_show(argv):
    path = None
    if "--file" in argv:
        path = argv[argv.index("--file") + 1]
    sectors = load_sectors(path)

    if 0 in sectors:
        print("=== sector 0: profile directory ===")
        for i, (sector, enabled, _) in enumerate(decode_directory(sectors[0])):
            print(f"  entry {i}: profile in sector {sector}  "
                  f"{'ENABLED' if enabled else 'disabled'} (flag {enabled:#04x})")
        print()

    for s in sorted(sectors):
        if s == 0:
            continue
        sec = sectors[s]
        if all(b == 0xFF for b in sec[:CRC_OFFSET]) or all(b == 0 for b in sec[:CRC_OFFSET]):
            continue
        if crc_ccitt_false(sec[:CRC_OFFSET]) != (sec[CRC_OFFSET] << 8) | sec[CRC_OFFSET + 1]:
            continue  # not a profile page
        print(f"=== sector {s}: profile ===")
        print(decode_profile(sec))
        print()


def build_rebind(sec, button, spec):
    """Return a new 256-byte sector with one button rebound and the CRC fixed."""
    if not 1 <= button <= 16:
        raise ValueError("button must be 1..16")
    new = bytearray(sec)
    off = BUTTONS_OFFSET + (button - 1) * BINDING_SIZE
    new[off:off + BINDING_SIZE] = encode_binding(spec)
    crc = crc_ccitt_false(new[:CRC_OFFSET])
    new[CRC_OFFSET] = crc >> 8
    new[CRC_OFFSET + 1] = crc & 0xFF
    return bytes(new)


def cmd_plan(argv, apply=False):
    if len(argv) < 3:
        sys.exit("usage: profile.py plan <sector> <button> <binding>")
    sector_id, button, spec = int(argv[0], 0), int(argv[1], 0), argv[2]

    with Device() as dev:
        old = read_sector(dev, sector_id, SECTOR_SIZE)
        new = build_rebind(old, button, spec)

        off = BUTTONS_OFFSET + (button - 1) * BINDING_SIZE
        print(f"sector {sector_id}, button {button} "
              f"({BUTTON_NAMES[button - 1] if button <= len(BUTTON_NAMES) else '?'})")
        print(f"  offset {off:#04x}")
        print(f"  before: {old[off:off + 4].hex(' ')}  {decode_binding(old[off:off + 4])}")
        print(f"  after:  {new[off:off + 4].hex(' ')}  {decode_binding(new[off:off + 4])}")
        print(f"  crc:    {old[CRC_OFFSET]:02x}{old[CRC_OFFSET + 1]:02x}"
              f" -> {new[CRC_OFFSET]:02x}{new[CRC_OFFSET + 1]:02x}")
        diff = [i for i in range(SECTOR_SIZE) if old[i] != new[i]]
        print(f"  bytes changed: {len(diff)} at {[hex(i) for i in diff]}")

        if not apply:
            print("\n(dry run - nothing written)")
            return

        if "--i-understand-this-writes-flash" not in argv:
            sys.exit("refusing to write without --i-understand-this-writes-flash")

        write_sector(dev, sector_id, new)
        print("\nwritten. re-reading to verify:")
        back = read_sector(dev, sector_id, SECTOR_SIZE)
        print(f"  {back[off:off + 4].hex(' ')}  {decode_binding(back[off:off + 4])}")
        print("  MATCH" if back == new else "  MISMATCH - restore from backup!")


def write_sector(dev, sector, data):
    """0x8100: fn6 memoryAddrWrite(sector, offset, length), fn7 memoryWrite(16 bytes),
    fn8 memoryWriteEnd. Sequence taken from libratbag's hidpp20 onboard profile writer."""
    if len(data) != SECTOR_SIZE:
        raise ValueError("sector must be exactly 256 bytes")
    idx = dev.feature_index(0x8100)
    # fn6 memoryAddrWrite: sector BE, sub-address BE (always 0), byte count BE.
    dev.request(idx, 6, bytes([sector >> 8, sector & 0xFF, 0, 0,
                               SECTOR_SIZE >> 8, SECTOR_SIZE & 0xFF]), long=True)
    # fn7 memoryWrite: 16 raw bytes at a time, sector_size/16 times.
    for off in range(0, SECTOR_SIZE, 16):
        dev.request(idx, 7, data[off:off + 16], long=True)
    # fn8 memoryWriteEnd is a SHORT report and is where the device reports invalid
    # data. It can return an error even though the bytes did land, so surface the
    # error rather than aborting - the caller verifies by reading the sector back.
    try:
        dev.request(idx, 8)
    except HidppError as exc:
        print(f"  memoryWriteEnd reported: {exc}")


def cmd_restore(argv):
    """Write sectors back from a backup made by profile_backup.py.

    This is the recovery path if a rebind goes wrong. Only sectors whose stored CRC
    validates are eligible, so a corrupt backup cannot be pushed to the device.
    """
    if not argv:
        sys.exit("usage: profile.py restore <backup.bin> [sector ...] "
                 "--i-understand-this-writes-flash")
    path = argv[0]
    wanted = [int(a, 0) for a in argv[1:] if not a.startswith("--")]

    with open(path, "rb") as fh:
        blob = fh.read()
    present = int.from_bytes(blob[-2:], "big")

    targets = []
    for s in range(16):
        if not present & (1 << s):
            continue
        if wanted and s not in wanted:
            continue
        sec = blob[s * SECTOR_SIZE:(s + 1) * SECTOR_SIZE]
        stored = (sec[CRC_OFFSET] << 8) | sec[CRC_OFFSET + 1]
        if crc_ccitt_false(sec[:CRC_OFFSET]) != stored:
            print(f"sector {s}: CRC invalid in backup, skipping")
            continue
        targets.append((s, sec))

    print(f"would restore sectors: {[s for s, _ in targets]}")
    if "--i-understand-this-writes-flash" not in argv:
        print("(dry run - nothing written)")
        return

    with Device() as dev:
        for s, sec in targets:
            write_sector(dev, s, sec)
            back = read_sector(dev, s, SECTOR_SIZE)
            print(f"sector {s}: {'OK' if back == sec else 'MISMATCH'}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    try:
        if cmd == "show":
            cmd_show(sys.argv[2:])
        elif cmd == "plan":
            cmd_plan(sys.argv[2:], apply=False)
        elif cmd == "apply":
            cmd_plan(sys.argv[2:], apply=True)
        elif cmd == "restore":
            cmd_restore(sys.argv[2:])
        else:
            sys.exit(__doc__)
    except HidppError as exc:
        sys.exit(f"error: {exc}")


if __name__ == "__main__":
    main()
