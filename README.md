# G502 Daemon

This repository contains a Linux daemon for mapping the Logitech G502 Hero mouse side buttons to keyboard modifiers.

## Installation

To install the G502 Daemon, follow these steps:

```bash
# Clone the repository
git clone https://github.com/wdhg/g502d.git
cd g502d

# Build the daemon
# Note, this will use the configuation defined in config.h
# You will likely need to update this file as it contains hardcoded keyboard vendor and model IDs
# (Unless you also happen to have a Unicomp Model M keyboard :P)
# This file also contains a fixed DPI scaling modifier which you may want to adjust
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
