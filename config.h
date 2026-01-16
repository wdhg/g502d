#ifndef CONFIG_H
#define CONFIG_H

// G502 identifiers
#define G502_USB_VENDOR_ID   0x046d
#define G502_USB_VENDOR_ID_S "046d"
#define G502_MODEL_ID        0xc332
#define G502_MODEL_ID_S      "c332"
// Keyboard identifiers
// Update these if your keyboard is different
// To find your keyboard event device IDs, find it in the list of input devices:
//     $ libinput list-devices
// To find your keyboard's USB vendor and model IDs:
//     $ udevadm info --query=all --name=/dev/input/eventX
#define KB_USB_VENDOR_ID   0x17f6
#define KB_USB_VENDOR_ID_S "17f6"
#define KB_MODEL_ID        0x0862
#define KB_MODEL_ID_S      "0862"

// DPI scaling factor (for converting G502 DPI to OS cursor speed)
#define DPI_SCALE 0.5

#endif // CONFIG_H
