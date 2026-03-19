#!/bin/bash

# BBS Module Flash Script
# Flashes firmware to connected Heltec V3 device

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
FIRMWARE_ROOT="$PROJECT_ROOT/firmware"
VENV_PATH="$PROJECT_ROOT/.venv"

echo "=== BBS Module Flash ==="
echo "Project root: $PROJECT_ROOT"
echo "Firmware root: $FIRMWARE_ROOT"
echo ""

# Activate virtual environment
if [ ! -d "$VENV_PATH" ]; then
    echo "Error: Python venv not found at $VENV_PATH"
    exit 1
fi

echo "Activating Python venv..."
source "$VENV_PATH/bin/activate"
echo "✓ Venv activated"
echo ""

# Flash firmware
cd "$FIRMWARE_ROOT"
echo "Flashing firmware to Heltec V3..."
echo "Command: pio run -e heltec-v3 -t upload"
echo ""
echo "Make sure your Heltec V3 is connected via USB..."
echo ""

if pio run -e heltec-v3 -t upload; then
    echo ""
    echo "=== Flash Successful ==="
    echo ""
    echo "Waiting for device to reboot..."
    sleep 3
    echo ""
    echo "Device is ready. To view logs:"
    echo "  pio device monitor -b 115200"
    exit 0
else
    echo ""
    echo "=== Flash Failed ==="
    echo "Check connection and try again."
    exit 1
fi
