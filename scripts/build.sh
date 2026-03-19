#!/bin/bash

# BBS Module Build Script
# Integrates module files and builds firmware for heltec-v3

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
FIRMWARE_ROOT="$PROJECT_ROOT/firmware"
VENV_PATH="$PROJECT_ROOT/.venv"

echo "=== BBS Module Build ==="
echo "Project root: $PROJECT_ROOT"
echo "Firmware root: $FIRMWARE_ROOT"
echo ""

# Activate virtual environment
if [ ! -d "$VENV_PATH" ]; then
    echo "Error: Python venv not found at $VENV_PATH"
    echo "Run: python3 -m venv $VENV_PATH"
    exit 1
fi

echo "Activating Python venv..."
source "$VENV_PATH/bin/activate"
echo "✓ Venv activated"
echo ""

# Run integration script
echo "Running integration script..."
bash "$SCRIPT_DIR/integrate.sh"
echo ""

# Build firmware
cd "$FIRMWARE_ROOT"
echo "Building firmware for heltec-v3..."
echo "Command: pio run -e heltec-v3"
echo ""

if pio run -e heltec-v3; then
    echo ""
    echo "=== Build Successful ==="
    echo "Binary location: $FIRMWARE_ROOT/.pio/build/heltec-v3/firmware.bin"
    echo ""
    echo "Next steps:"
    echo "  To flash: ./scripts/flash.sh"
    echo "  To monitor: pio device monitor -b 115200"
    exit 0
else
    echo ""
    echo "=== Build Failed ==="
    echo "Please check the error messages above and review the firmware source."
    exit 1
fi
