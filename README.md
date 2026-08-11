# G502 Daemon

This repository contains a Linux daemon for mapping gaming-mouse side buttons to keyboard
modifiers, so they can modify keyboard input under Wayland.

Mice are listed in `MOUSE_DEVICES` in `config.h` and tried in order; the first one connected
wins. Two are set up:

| Mouse | USB ID | Working side buttons |
| --- | --- | --- |
| Razer Basilisk V3 Pro | `1532:00aa` wired | 3 of 3, after an onboard rebind |
| G502 Proteus Spectrum | `046d:c332` | 3 of 3, after an onboard rebind |

The G502 work was done against a **Proteus Spectrum** (firmware `U1 03.02`, which reports its
own name as "Tunable RGB Gaming Mouse G502"). The G502 Hero is a different USB ID
(`046d:c08b`); the button layout is the same but none of the memory offsets below have been
checked on one.

Only one mouse is driven at a time. A second connected mouse keeps working as an ordinary
pointer - it just does not get the side-button mapping or the debouncing.

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

## The Razer Basilisk V3 Pro

Its two thumb buttons report the same HID usages as the G502's - `0x00090004` as `BTN_SIDE`
and `0x00090005` as `BTN_EXTRA` - so `MODIFIER_BUTTON_MAP` drives them unchanged, giving
Left Shift and Left Ctrl.

The sniper paddle sends nothing, on any of its four interfaces. As on the G502, the firmware
is consuming it: by default it is bound to Razer's "Sensitivity Clutch", which is handled
inside the mouse. Verify with:

    ./tools/identify_buttons.py razer

**The G502 fix does not transfer.** There we rebound the paddle to HID mouse button 6, which
worked because the G502's descriptor already declared sixteen buttons. The Basilisk's mouse
interface declares five:

    B: KEY=1f0000      # BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA

The kernel builds that bitmap from the HID report descriptor, so the mouse *cannot* report a
sixth button whatever the onboard profile says. The paddle has to be bound to a **keyboard
key** instead, which the mouse emits on its keyboard interface (`event22`/`event23`).

Convenient side effect: a key from a keyboard interface is a real keypress as far as Wayland
is concerned, so binding the paddle straight to Left Alt works with or without this daemon.
The daemon grabs those interfaces and forwards them anyway, so it behaves the same either way.

### Rebinding the paddle

`tools/razer.py` does this over Razer's onboard protocol. Nothing off the shelf can: openrazer
has no button-remapping code at all (issues [#2031][or2031], [#2688][or2688]) and libratbag has
no Razer driver ([#103][lr103]).

    sudo ./tools/razer.py info                                    # prove the transport works
    sudo ./tools/razer.py read 15                                 # the paddle's slot
    sudo ./tools/razer.py backup ~/.local/share/g502d/razer.json
    sudo ./tools/razer.py set-key 15 lalt                         # dry run, prints the diff
    sudo ./tools/razer.py set-key 15 lalt --i-understand-this-writes-onboard-memory

The paddle then reports Left Alt on the mouse's keyboard interface, which Wayland accepts as a
genuine modifier - so it works with or without this daemon. To undo it:

    sudo ./tools/razer.py restore ~/.local/share/g502d/razer.json --i-understand-this-writes-onboard-memory

**Caveat:** as with the G502, this lives in the mouse. It survives reboots, applies to Windows
too, and Synapse may overwrite it.

### The protocol

Razer's is a 90-byte HID feature report, unrelated to HID++. Framing and CRC are from
openrazer's [`razercommon.h`][rzh] / [`.c`][rzc]; the button-assignment command class is from
[OpenSnek][snek], which reverse-engineered it for macOS.

    byte 0      status (response: 0x02 success, 0x01 busy, 0x03 failure, 0x05 unsupported)
    byte 1      transaction id - 0x1f for the Basilisk V3 Pro
    bytes 2-3   remaining packets, big-endian
    byte 4      protocol type
    byte 5      data size
    byte 6      command class
    byte 7      command id
    bytes 8-87  arguments
    byte 88     CRC: XOR of bytes 2..87
    byte 89     reserved

Sent as a feature report, report id 0, on USB interface 0. Button assignment is command class
`0x02`, id `0x0c` to write and `0x8c` to read, with arguments `[profile, slot, hypershift]`
followed by a 7-byte function block:

    00 00 ...                    disabled
    01 01 <button>               HID mouse button
    02 02 <modifiers> <usage>    HID keyboard key
    06 01 <action> <dpi be16 x2> device-internal action
    0d 04 <mods> <usage> <rate>  keyboard key, turbo
    0e 03 <button> <rate be16>   mouse button, turbo

Modifier bits are Ctrl `0x01`, Shift `0x02`, Alt `0x04`, GUI `0x08`, right-hand variants in the
high nibble. The key is a standard HID keyboard usage, so Left Alt is `0xe2` and F13-F24 are
`0x68`-`0x73`.

Slots on this mouse, from `./tools/razer.py dump`. Reading a slot that does not exist returns
status `0x03`, which is a useful way to map the table:

| Slot | Factory binding | |
| --- | --- | --- |
| 1-5 | `01 01 0n` | mouse buttons 1-5 |
| 9, 10 | `01 01 09` / `01 01 0a` | mouse buttons 9 and 10 |
| 14 | `07 01 04` | undocumented class `0x07` |
| 15 | `06 01 05 01 90 01 90` | sensitivity clutch, 400 DPI on both axes |
| 52, 53, 96 | | present; not investigated |

Slot 15's trailing `01 90 01 90` is two big-endian `0x0190` = 400, the DPI the clutch drops to.
That is the whole reason the paddle was silent: the firmware acts on it internally and puts
nothing on the wire.

[or2031]: https://github.com/openrazer/openrazer/issues/2031
[or2688]: https://github.com/openrazer/openrazer/issues/2688
[lr103]: https://github.com/libratbag/libratbag/issues/103
[rzh]: https://github.com/openrazer/openrazer/blob/master/driver/razercommon.h
[rzc]: https://github.com/openrazer/openrazer/blob/master/driver/razercommon.c
[snek]: https://github.com/gh123man/OpenSnek

## Debouncing a worn switch

The left switch on this mouse developed contact bounce: on **release**, the
contacts re-close for a few tens of milliseconds as the leaf springs back, and
the host reads that as a second click. Measured on `/dev/input/event2` with the
daemon stopped, so this is the hardware, not the daemon:

    48.071  up      held 300 ms      <- release
    48.093  down    22 ms later      <- switch re-closes on its own
    48.115  up      held 22 ms

It is intermittent, it is tied to the release edge rather than to fast clicking
(the press above was held 300 ms), and it is the classic failure mode of the
D2FC-F-7N switches these mice use.

It only became visible once this daemon was running. libinput does its own
button debouncing and tightens its filter on a device once it has seen that
device bounce, and it keeps that state while the device stays open. The physical
mouse has been open since boot and had learned it; the virtual device is
destroyed and recreated on every daemon restart, so it starts naive and passes
the bounce straight through.

The same switch also chatters the other way, flicking open for a moment while
the button is still held. That reaches the host as the end of a drag.

Both faults are the same event pair - a release followed closely by a press -
and at the moment the release arrives there is no way to tell it from a real
one. So `CLICK_DEBOUNCE_MS` in `config.h` holds every release back for that
long. If the button goes down again inside the window, the release is cancelled
and the press is swallowed: the host never sees the button come up, which is
what actually happened in both cases.

Dropping the *press* instead is the obvious implementation and it does fix
double clicks, but it breaks drags - the spurious release has already gone out,
the recovery press gets eaten as bounce, and the button is left stuck up.

Measured bounce sat at 22-30 ms from the edge, against 672-3965 ms between
genuine repeat clicks, so the window has two orders of magnitude of room. The
cost is up to `CLICK_DEBOUNCE_MS` of latency on release; presses are never
delayed.

Every drop is logged, so the journal tells you the real bounce rate and whether
the window needs adjusting:

    journalctl --user -u g502d -f | grep Debounce

If real clicks start going missing, lower it. If doubles still get through,
raise it. Setting it to 0 disables debouncing entirely.

This is a workaround for failing hardware; the permanent fix is replacing the
switch. Debouncing in software is what mouse firmware does anyway.

## Status bar module

`tools/waybar-status.py` is a waybar module reporting battery, DPI, polling rate and -
the part `systemctl status` cannot tell you - whether input is actually being routed.

The daemon only does anything while it holds an `EVIOCGRAB` on the physical devices. A
daemon that is "active" but grabbing nothing looks healthy and does nothing, so the module
reads the daemon's own open file descriptors (`/proc/<pid>/fd`, readable without root since
it runs as your user) and lists exactly which devices are captured.

| State | Meaning |
| --- | --- |
| `routed` | Holding grabs, virtual devices present - working |
| `degraded` | Running, but capturing nothing; input reaches the compositor directly |
| `off` | Service stopped |
| `low` | Routed, on battery at or below 15% |

The bar shows the charge only when the mouse is on battery. Wired, it reads a permanent
100% charging, so the module shows DPI instead and the tooltip just says `power: wired`.
Transport is identified by USB product id - this mouse enumerates differently per connection
mode - rather than by the charging flag, which is also set while a wireless mouse charges.

Left click toggles the daemon, right click opens the full status in a terminal. Both go
through `tools/g502d-ctl.sh`, which also signals waybar to refresh rather than waiting for
the next poll.

    ./tools/waybar-status.py --plain      # what the tooltip shows
    ./tools/razer.py status               # device stats on their own

`install.sh` copies these into `~/.local/bin`. Reading battery and DPI needs hidraw access:

    sudo cp udev/99-razer-hidraw.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw

Without that rule everything else still works and the stats are simply omitted.

The waybar side is two edits to `~/.config/waybar/`. In `config.jsonc`, add `"custom/g502d"`
to `modules-right` and define it:

```jsonc
"custom/g502d": {
  "format": "{icon} {text}",
  "format-icons": {
    "routed": "\udb80\udf7d",
    "degraded": "\udb82\udd9e",
    "low": "\udb80\udf7d",
    "off": "\udb80\udf7e"
  },
  "tooltip": true,
  "interval": 30,
  "return-type": "json",
  "exec": "$HOME/.local/bin/g502d-status",
  "on-click": "$HOME/.local/bin/g502d-ctl toggle",
  "on-click-right": "omarchy-launch-floating-terminal-with-presentation $HOME/.local/bin/g502d-ctl show",
  "signal": 9
}
```

Use `{text}`, not `{}`. Mixing a named placeholder with a positional one is rejected by
current fmt, and waybar responds by dropping the module completely - the bar renders fine
with the module simply absent, which looks like the config never loaded. `waybar -l trace`
prints the actual error.

Then in `style.css`, dim the module when input is bypassing the daemon:

```css
#custom-g502d { min-width: 12px; margin: 0 7.5px; }
#custom-g502d.off, #custom-g502d.degraded { opacity: 0.5; }
#custom-g502d.low { color: #a55555; }
```

Waybar does not auto-reload - run `omarchy restart waybar`, and make sure the config write
has actually landed first.

## Hotplug

Devices are enumerated at startup *and* watched afterwards via a `sd_device_monitor` on the
`input` subsystem. A keyboard connected later is grabbed as it appears; before this, it was
ignored until the daemon was restarted, which mattered because a mouse that sends keystrokes
presents itself as a keyboard.

The keyboard registry is append-only, so pointers held by running threads stay valid as new
entries arrive. Removal is already handled by each keyboard thread, which re-resolves its
device node by syspath when a read fails.

A missing mouse is no longer fatal at startup: the mouse thread waits for one of the
configured devices to appear.

Two failure modes that only show up when switching a mouse between connection modes:

**Preferred-mouse recovery.** When the mouse in use disappears, the search falls through to
the next entry in `MOUSE_DEVICES` - reasonable - but it would then keep that lesser mouse
forever, because a working device never errors and so never triggers another search. Plugging
the Basilisk's dongle in left the daemon driving the G502. The udev monitor now compares the
new device's rank against the one in use and pokes a pipe the mouse thread polls alongside
the device, so it re-picks immediately.

**Keyboards that never come back.** A replugged device reappears under a *new* syspath, so
`refresh_keyboard_devnode()` could never find the old one again. The thread retried forever,
one failed `read()` on a closed fd every six seconds - 20 log lines a minute per dead
keyboard. Threads now give up after `KEYBOARD_REOPEN_ATTEMPTS` and release their slot for
reuse; the monitor adds the replacement as a new entry.

## Wireless

The Basilisk V3 Pro reports a **different USB product ID per connection mode**, so each mode
you use has to be listed in `MOUSE_DEVICES`. Confirmed: `1532:00aa` wired and `1532:00ab`
over the HyperSpeed 2.4GHz dongle. The Bluetooth id is still a guess.

Note the dongle is a USB device in its own right - `ID_BUS=usb`, bus `0003`. Only a real
Bluetooth connection appears on bus `0005`, so "wireless" and "Bluetooth" are not the same
thing here, and `tools/razer.py` works fine over the dongle. Its first command after the
mouse has been idle may fail while the mouse wakes up; run it again.

The dongle presents **two** `ID_INPUT_MOUSE` nodes with different capabilities - one with 5
buttons and wheel tilt, one with 6 buttons and no tilt. Only one carries movement. Ties in
`find_mouse_device()` keep the first node enumerated; confirm with
`./tools/identify_buttons.py razer` if the pointer ever goes dead after a mode switch.

Device matching is deliberately not based on udev's `ID_USB_VENDOR_ID` / `ID_MODEL_ID`
properties, because those exist only for USB devices - over Bluetooth the same mouse arrives
on the bluetooth bus with no USB properties at all and would never be found. It matches the
input device's own `id/vendor` and `id/product` sysattrs instead, which every bus populates:

    cat /sys/class/input/event21/device/id/{vendor,product}

So the daemon's side-button mapping and debouncing work over any transport, given the right
IDs in the config. To find them for a mode you have not used yet, connect that way and run
`lsusb`, or read the sysattrs above.

The onboard button bindings live in the mouse, so the paddle keeps sending Left Alt over
every transport regardless of any of this.

**`tools/razer.py` is a different matter.** It talks to a USB HID control interface, so it
works wired and over the 2.4GHz dongle but is unlikely to work over Bluetooth - openrazer
does not support Bluetooth for these devices either. In practice that means battery and DPI
in the status module, and any rebinding, need USB. Since rebinding is a one-off and persists
in hardware, that is mostly a limitation on the stats.

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
| `tools/identify_buttons.py` | Watch every interface of a device and name what each button sends |
| `tools/razer.py` | Razer onboard protocol: status, read and rebind button slots |
| `tools/waybar-status.py` | Waybar module: device stats and whether input is routed |
| `tools/g502d-ctl.sh` | Toggle the daemon and refresh the bar |
| `tools/clickcount.py` | Count press/release pairs on an evdev node, flag duplicates |
| `tools/clicktest.html` | Same, but at the application layer - open it in a browser |

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
