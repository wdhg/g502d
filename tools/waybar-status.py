#!/usr/bin/env python3
"""Waybar module for g502d: device stats and where input is actually flowing.

Prints one line of waybar JSON. The interesting part is `routing`: the daemon
only does anything if it holds an EVIOCGRAB on the physical devices, so this
inspects the daemon's own open file descriptors rather than trusting that the
service being "active" means it is working.

    ./tools/waybar-status.py            # waybar JSON
    ./tools/waybar-status.py --plain    # human-readable

Device stats need read access to the Razer hidraw node; see
udev/99-razer-hidraw.rules. Without it, everything else still works and the
stats are simply omitted.
"""

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import razer
except ImportError:
    razer = None

SERVICE = "g502d"

# Names the daemon gives its uinput devices, from config.h
VIRTUAL_MOUSE = "Virtual G502 Hero"
VIRTUAL_KEYBOARD = "Virtual Keyboard"


def service_state():
    """(ActiveState, MainPID) for the user service."""
    try:
        out = subprocess.run(
            ["systemctl", "--user", "show", SERVICE,
             "--property=ActiveState", "--property=MainPID"],
            capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return "unknown", 0

    fields = dict(line.split("=", 1) for line in out.strip().splitlines() if "=" in line)
    try:
        pid = int(fields.get("MainPID", "0"))
    except ValueError:
        pid = 0
    return fields.get("ActiveState", "unknown"), pid


def input_devices():
    """{'/dev/input/eventN': name} for everything on the system."""
    devices, name = {}, None
    try:
        with open("/proc/bus/input/devices") as handle:
            for raw in handle:
                line = raw.strip()
                if line.startswith('N: Name="'):
                    name = line[9:-1]
                elif line.startswith("H: Handlers="):
                    for handler in line[12:].split():
                        if handler.startswith("event"):
                            devices["/dev/input/" + handler] = name or "?"
    except OSError:
        pass
    return devices


def captured_devices(pid):
    """The event devices the daemon currently holds open.

    The daemon runs as the user, so its /proc/<pid>/fd is readable without
    root. An fd here means the device is grabbed and its events are being
    rewritten rather than reaching the compositor directly.
    """
    if not pid:
        return []
    captured = []
    fd_dir = f"/proc/{pid}/fd"
    try:
        for entry in os.listdir(fd_dir):
            try:
                target = os.readlink(os.path.join(fd_dir, entry))
            except OSError:
                continue
            if target.startswith("/dev/input/event"):
                captured.append(target)
    except OSError:
        return []
    return sorted(set(captured))


def device_stats():
    """Battery, DPI and polling rate, or {} if the mouse cannot be reached."""
    if razer is None:
        return {}
    try:
        with razer.Device() as dev:
            stats = {"wired": dev.is_wired()}
            for name, getter in (("battery", dev.battery_percent),
                                 ("charging", dev.is_charging),
                                 ("dpi", dev.dpi),
                                 ("polling", dev.polling_rate)):
                try:
                    stats[name] = getter()
                except razer.RazerError:
                    stats[name] = None
            return stats
    except razer.RazerError:
        return {}
    except Exception:  # never let the bar module crash on an unexpected fault
        return {}


def build():
    state, pid = service_state()
    devices = input_devices()
    captured = captured_devices(pid)
    virtual_present = VIRTUAL_MOUSE in devices.values()

    stats = device_stats()

    # Routing: a grabbed physical device plus a virtual one means we are in the
    # path. Active but grabbing nothing means the daemon is up and doing nothing,
    # which looks identical from `systemctl status` and is worth surfacing.
    if state != "active":
        status = "off"
    elif captured and virtual_present:
        status = "routed"
    else:
        status = "degraded"

    lines = []
    if status == "off":
        lines.append(f"g502d: {state} - input goes straight to the compositor")
    elif status == "degraded":
        lines.append("g502d: running but capturing nothing")
        lines.append("input is reaching the compositor directly")
    else:
        lines.append(f"g502d: routing through virtual devices (pid {pid})")

    if captured:
        lines.append("")
        lines.append("captured:")
        for path in captured:
            lines.append(f"  {devices.get(path, '?')} ({os.path.basename(path)})")

    if stats:
        lines.append("")
        if stats.get("wired") and stats.get("battery") is not None:
            # Worth keeping the number while wired: it is how you tell a topped-up
            # mouse from one still charging.
            state = "fully charged" if stats["battery"] >= 100 else "charging"
            lines.append(f"power: wired, {stats['battery']}% ({state})")
        elif stats.get("wired"):
            lines.append("power: wired")
        elif stats.get("battery") is not None:
            charging = " charging" if stats.get("charging") else ""
            lines.append(f"battery: {stats['battery']}%{charging}")
        if stats.get("dpi"):
            x, y = stats["dpi"]
            lines.append(f"dpi: {x}" + (f" x {y}" if x != y else ""))
        if stats.get("polling"):
            lines.append(f"polling: {stats['polling']} Hz")

    # Show the charge whenever there is one, wired included - seeing it reach
    # 100% is the point. DPI is the fallback when there is no battery at all.
    on_battery = not stats.get("wired") and stats.get("battery") is not None
    if stats.get("battery") is not None:
        text = f"{stats['battery']}%"
    elif stats.get("dpi"):
        text = f"{stats['dpi'][0]}"
    else:
        text = ""

    if status == "routed" and on_battery and stats["battery"] <= 15:
        status = "low"

    return {
        "text": text,
        "alt": status,
        "class": status,
        "tooltip": "\n".join(lines),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--plain", action="store_true", help="human-readable output")
    args = parser.parse_args()

    payload = build()
    if args.plain:
        print(payload["tooltip"])
        print(f"\n[{payload['alt']}] {payload['text']}")
    else:
        print(json.dumps(payload))


if __name__ == "__main__":
    main()
