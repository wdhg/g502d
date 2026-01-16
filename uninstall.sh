#!/bin/bash

INSTALL_DIR="$HOME/.local/bin"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_NAME="g502d.service"
PROG_NAME="g502d"

# Stop and disable the systemd service
echo "Stopping and disabling $SERVICE_NAME..."
systemctl --user stop "$SERVICE_NAME"
systemctl --user disable "$SERVICE_NAME"
echo "$SERVICE_NAME has been stopped and disabled."

# Remove the systemd service file
echo "Removing systemd service file..."
rm "$SERVICE_DIR/$SERVICE_NAME"
echo "Removed $SERVICE_NAME from $SERVICE_DIR."

# Remove the program from the installation directory
echo "Removing $PROG_NAME from $INSTALL_DIR..."
rm "$INSTALL_DIR/$PROG_NAME"
echo "Removed $PROG_NAME from $INSTALL_DIR."
echo "Uninstallation complete."
