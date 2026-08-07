#ifndef CONFIG_H
#define CONFIG_H

// G502 identifiers
#define G502_USB_VENDOR_ID   0x046d
#define G502_USB_VENDOR_ID_S "046d"
#define G502_MODEL_ID        0xc332
#define G502_MODEL_ID_S      "c332"
// Virtual keyboard identifiers.
// These are only what the virtual device reports about itself; keyboards are
// discovered at runtime via udev's ID_INPUT_KEYBOARD tag, so there is nothing to
// configure here even if you have several. See find_keyboard_devices().
#define KB_USB_VENDOR_ID 0x1d6b // Linux Foundation
#define KB_MODEL_ID      0x0001

// Mouse buttons that are re-emitted as keyboard keys on the virtual keyboard
// instead of being forwarded to the virtual mouse.
//
//     X(mouse button code, keyboard key code, mouse scan code, keyboard scan code)
//
// Scan codes are HID usages. Mouse buttons live on usage page 0x09, so physical
// button N reports 0x000900NN. Keyboard keys live on usage page 0x07, where the
// eight modifiers occupy 0xE0..0xE7 (LeftCtrl, LeftShift, LeftAlt, LeftGUI, ...).
//
// BTN_FORWARD is the thumb paddle. It only reports anything once the onboard
// profile binds physical button 6 to a HID mouse button; out of the box the
// firmware keeps that button for its internal DPI-shift action and sends nothing.
// See tools/profile.py.
// BTN_BACK and BTN_TASK are the two buttons behind the wheel. They are mapped to
// F13 and F14: keys that exist in evdev but on no physical keyboard, so nothing
// claims them by default and they are free to bind in a compositor or app.
#define MODIFIER_BUTTON_MAP                                  \
	X(BTN_SIDE,    KEY_LEFTSHIFT, 0x00090004, 0x000700E1) \
	X(BTN_EXTRA,   KEY_LEFTCTRL,  0x00090005, 0x000700E0) \
	X(BTN_FORWARD, KEY_LEFTALT,   0x00090006, 0x000700E2) \
	X(BTN_BACK,    KEY_F13,       0x00090007, 0x00070068) \
	X(BTN_TASK,    KEY_F14,       0x00090008, 0x00070069)

// DPI scaling factor (for converting G502 DPI to OS cursor speed)
#define DPI_SCALE 0.5

// Contact bounce window, in milliseconds. Button releases are held back this
// long; if the button goes down again before the window expires, the release
// and the new press are both discarded, because the button never really came
// up. Set to 0 to disable. See the comment above pending_releases in g502d.c.
//
// The cost is up to this much latency on every button release, so keep it as
// small as the hardware allows. Measured bounce on this mouse sat at 22-30 ms
// from the edge; genuine repeat clicks were hundreds of milliseconds apart, so
// there is a lot of room between the two. Raise it if doubles or dropped drags
// still get through, lower it if releases feel sluggish. Every suppressed
// bounce is logged (journalctl --user -u g502d), so the log tells you the real
// bounce rate rather than leaving you to guess.
#define CLICK_DEBOUNCE_MS 40

#endif // CONFIG_H
