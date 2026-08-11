#!/bin/bash

# Install as a systemd service
INSTALL_DIR="$HOME/.local/bin"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_NAME="g502d.service"
PROG_NAME="g502d"

echo "*************"
echo "* IMPORTANT *"
echo "*************"

echo ""
echo "Using the keyboard/mouse while this installer is running may cause stuck keys."
echo "Please refrain from using the keyboard/mouse until the installation is complete."
echo "Press [ENTER] to continue..."
read

# Copy the program to the installation directory
echo "Installing $PROG_NAME to $INSTALL_DIR..."
cp "$PROG_NAME" "$INSTALL_DIR/$PROG_NAME"
chmod +x "$INSTALL_DIR/$PROG_NAME"
echo "Installed $PROG_NAME."

# Create the systemd service file
echo "Copying systemd service file..."
mkdir -p "$SERVICE_DIR"
cp "$SERVICE_NAME" "$SERVICE_DIR/$SERVICE_NAME"
echo "Copied $SERVICE_NAME to $SERVICE_DIR."

# Enable and start the service
echo "Enabling and starting $SERVICE_NAME..."
systemctl --user daemon-reload
systemctl --user enable "$SERVICE_NAME"
systemctl --user start "$SERVICE_NAME"
echo "$SERVICE_NAME is now enabled and started."

# Install the helper scripts the waybar module uses. razer.py goes alongside
# g502d-status because the status module imports it for battery and DPI.
echo "Installing helper scripts to $INSTALL_DIR..."
cp tools/razer.py "$INSTALL_DIR/razer.py"
cp tools/waybar-status.py "$INSTALL_DIR/g502d-status"
cp tools/g502d-ctl.sh "$INSTALL_DIR/g502d-ctl"
chmod +x "$INSTALL_DIR/razer.py" "$INSTALL_DIR/g502d-status" "$INSTALL_DIR/g502d-ctl"

# Device stats need hidraw access, which is root-only by default.
if [ ! -f /etc/udev/rules.d/99-razer-hidraw.rules ] && [ -f udev/99-razer-hidraw.rules ]; then
	echo ""
	echo "To read battery and DPI without root, install the udev rule:"
	echo "  sudo cp udev/99-razer-hidraw.rules /etc/udev/rules.d/"
	echo "  sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw"
	echo ""
fi

echo "Wait a few seconds for the virtual devices to be created."
sleep 5
echo "Installation complete!"
