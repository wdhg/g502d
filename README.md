# G502 Daemon

This repository contains a Linux daemon for mapping the Logitech G502 side buttons to keyboard modifiers.

Developed against a **G502 Proteus Spectrum** (USB `046d:c332`, firmware `U1 03.02`, which
reports its own name as "Tunable RGB Gaming Mouse G502"). The G502 Hero is a different USB
ID (`046d:c08b`); the button layout is the same but none of the memory offsets below have
been checked on one.

## Installation

To install the G502 Daemon, follow these steps:

```bash
# Clone the repository
git clone https://github.com/wdhg/g502d.git
cd g502d

# Build the daemon
# Note, this will use the configuation defined in config.h
# Keyboards are detected automatically, so there is nothing to configure for them.
# config.h holds the button-to-key mapping and a fixed DPI scaling modifier,
# both of which you may want to adjust
./build.sh

# I recommend testing the daemon before installing it as a service
./g502d

# Install the daemon
./install.sh

# This should install and start the g502d service

# Uninstall the daemon
./uninstall.sh
```

## Why this is needed

There are two issues I encountered while trying to use the Logitech G502 Hero mouse on Linux:

1. Programs using `libratbag` (e.g. `piper`, `solaar`) don't seem to support macros with partial key presses, e.g. key down on side button press, key up on side button release. This functionality is needed to map side buttons to keyboard modifiers (e.g. Ctrl, Shift, Alt).
2. Whilst I had it working in X11, in Wayland it seems that mouse events cannot modifiy keyboard inputs. This means that even if you get the side buttons to send modifier key presses, they won't modify keyboard key presses. This is probably a security feature of Wayland.

This daemon works around both issues by capturing both keyboard and mouse events and mapping them to virtual input devices via `uinput`. This way we can transmit input modifiers from the mouse thread to the keyboard thread.

## The third side button

Out of the box the large thumb paddle (the "sniper" button) produces **no input event at
all** on Linux - not an evdev event, not even a HID report. This is not a driver problem and
no kernel change can fix it.

The mouse stores three profiles in onboard flash. Each profile has an 11-entry button table,
and in the factory profile the paddle's entry is:

    offset 0x34:  90 07 ff ff

`0x90` means "device-internal special action" and `0x07` is DPI-shift. The firmware handles
that press itself and never puts anything on the wire, so there is nothing for the host to
receive. Buttons 4 and 5 are `80 01 00 08` / `80 01 00 10`, i.e. real HID mouse buttons 4 and
5, which is why only those two ever worked.

The fix is to rebind the paddle to a real HID mouse button:

    ./tools/profile.py show                  # decode the onboard profiles
    ./tools/profile_backup.py save backup.bin
    ./tools/profile.py plan 1 6 mouse:6      # dry run, prints the exact byte diff
    ./tools/profile.py apply 1 6 mouse:6 --i-understand-this-writes-flash

That rewrites 6 bytes (the 4-byte binding plus the sector CRC). Button 6 then arrives as
`BTN_FORWARD`, which `config.h` maps to Left Alt.

To undo it:

    ./tools/profile.py restore backup.bin 1 --i-understand-this-writes-flash

**Caveat:** this lives in the mouse, not on the host, so it persists across reboots and
applies to Windows too. Logitech G HUB may rewrite profile 1 and undo it.

### Current mapping

| Physical button | Onboard binding | evdev code | Key sent | Compositor keysym |
| --- | --- | --- | --- | --- |
| thumb rear | `80 01 00 08` | `BTN_SIDE` | Left Shift | `Shift_L` |
| thumb front | `80 01 00 10` | `BTN_EXTRA` | Left Ctrl | `Control_L` |
| thumb paddle | `80 01 00 20` | `BTN_FORWARD` | Left Alt | `Alt_L` |
| behind wheel, upper | `80 01 00 40` | `BTN_BACK` | F13 | `XF86Tools` |
| behind wheel, lower | `80 01 00 80` | `BTN_TASK` | F14 | `XF86Launch5` |

The last two are deliberately bound to keys no physical keyboard has, so nothing claims them
and they are free to bind. **Bind them by their XF86 keysym, not as `F13`/`F14`**: with the
`pc` model and `evdev` keycodes, xkeyboard-config maps keycodes 183/184 to multimedia keysyms
rather than to `F13`/`F14`. Check yours with:

    xkbcli compile-keymap --layout gb --variant mac | grep 'key <FK1[34]>'

In Hyprland that means:

    bind = , XF86Tools,   exec, <command>
    bind = , XF86Launch5, exec, <command>

Rebinding the paddle and the two behind-wheel buttons removes all on-device DPI control, so
DPI is fixed at the profile default. Restore from a backup to get it back.

### Recovery

The bindings live in the mouse, not in this repo, so reinstalling the daemon will not
restore them. If G HUB, a firmware update or a factory reset wipes profile 1, reapply all
three in one go:

    ./tools/profile.py apply 1 6 mouse:6 --i-understand-this-writes-flash   # thumb paddle
    ./tools/profile.py apply 1 7 mouse:7 --i-understand-this-writes-flash   # behind wheel, upper
    ./tools/profile.py apply 1 8 mouse:8 --i-understand-this-writes-flash   # behind wheel, lower

Check the result with `./tools/profile.py show`. Buttons 6, 7 and 8 should read
`80 01 00 20`, `80 01 00 40` and `80 01 00 80`.

To go back to the factory layout, including on-device DPI control:

    ./tools/profile_backup.py save /tmp/before-restore.bin      # keep the current state first
    ./tools/profile.py restore <backup>.bin 1 --i-understand-this-writes-flash

Take a backup before any of this, and keep it somewhere outside the repo:

    ./tools/profile_backup.py save ~/.local/share/g502d/onboard-$(date +%F).bin

If the mouse ends up in a bad state, unplugging it clears anything volatile (button spy,
host mode). Feature `0x1802 DEVICE_RESET` is the last resort.

### Reading buttons without writing flash

HID++ feature `0x8110 MOUSE_BUTTON_SPY` reports a raw 16-bit button bitmap that ignores
profile bindings entirely, so it can observe buttons the firmware consumes internally:

    ./tools/button_spy.py 30

This needs no flash write and is how you can see the paddle, the two DPI buttons and the
profile-switch button. It only *observes* though - the on-device action still fires, so a
paddle press would still change DPI. That is why the profile rewrite is the better fix.

## Tools

All of these talk to the mouse over `/dev/hidraw1` using HID++ 2.0. Every write path is
gated behind an explicit flag.

| Tool | Purpose |
| --- | --- |
| `tools/hidpp.py` | HID++ 2.0 client: enumerate features, dump onboard memory |
| `tools/profile.py` | Decode / rebind / restore onboard profiles |
| `tools/profile_backup.py` | Dump and inspect all 16 memory sectors |
| `tools/button_spy.py` | Watch raw button state via feature `0x8110` |
| `tools/hidraw_watch.py` | Capture raw HID reports from both interfaces |

### Onboard profile format

Recovered from this device and cross-checked against libratbag and cvuchener/hidpp
("profile format 2 / memory model 1"). Sector 0 is the profile directory; profiles 1-3 live
in sectors 1-3, with read-only factory copies at `0x0101`-`0x0103`. Each sector is 256 bytes:

    0x00  report rate (ms)      0xa0  name, 48 bytes UTF-16LE
    0x01  default DPI index     0xd0  logo LED block
    0x02  shift DPI index       0xdb  side LED block
    0x03  DPI slots, 5 x u16 LE 0xfe  CRC-16/CCITT-FALSE over 0x00..0xfd, big-endian
    0x0d  profile colour RGB
    0x20  buttons, 16 x 4 bytes
    0x60  G-shift buttons, 16 x 4 bytes

A button binding is 4 bytes:

    80 01 <u16 BE mask>     HID mouse button (bit 0 = button 1)
    80 02 <mods> <usage>    HID keyboard: modifier bitmask + HID usage
    80 03 <u16 BE usage>    HID consumer control
    90 <action> <rsv> <prof>  device-internal action (0x07 = DPI shift)
    ff ff ff ff             disabled

Note the mouse button mask is big-endian while the DPI slots are little-endian.
