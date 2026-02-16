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

echo "Wait a few seconds for the virtual devices to be created."
sleep 5
echo "Installation complete!"
