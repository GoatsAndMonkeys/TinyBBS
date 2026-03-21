#!/bin/bash

# BBS Module Integration Script
# Copies module source files into the firmware tree and patches Modules.cpp

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_SRC="$PROJECT_ROOT/module-src"
FIRMWARE_ROOT="$PROJECT_ROOT/firmware"
FIRMWARE_MODULES="$FIRMWARE_ROOT/src/modules"
MODULES_CPP="$FIRMWARE_MODULES/Modules.cpp"

echo "=== BBS Module Integration ==="
echo "Project root: $PROJECT_ROOT"
echo "Module source: $MODULE_SRC"
echo "Firmware modules: $FIRMWARE_MODULES"
echo ""

# Check that source files exist
if [ ! -d "$MODULE_SRC" ]; then
    echo "Error: Module source directory not found: $MODULE_SRC"
    exit 1
fi

if [ ! -d "$FIRMWARE_MODULES" ]; then
    echo "Error: Firmware modules directory not found: $FIRMWARE_MODULES"
    exit 1
fi

# Copy module source files
echo "Copying module source files..."
cp "$MODULE_SRC/BBSStorage.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSStoragePSRAM.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSStorageLittleFS.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSWordle.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSModule_v2.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSModule_v2.cpp" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/FalloutWastelandRPG.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/FalloutWastelandRPG.cpp" "$FIRMWARE_MODULES/"
echo "✓ Module files copied"

# Patch Modules.cpp if needed
echo ""
echo "Patching Modules.cpp..."

# Remove old patches if they exist
if grep -q "#include \"BBSModule_v2.h\"" "$MODULES_CPP"; then
    # Remove old include (it might be in wrong place)
    sed -i '' '/#include "BBSModule_v2.h"/d' "$MODULES_CPP"
    echo "ℹ Removed old BBS include"
fi

if grep -q "bbsModule = new BBSModule()" "$MODULES_CPP"; then
    # Remove old instantiation
    sed -i '' '/bbsModule = new BBSModule()/d' "$MODULES_CPP"
    echo "ℹ Removed old BBS instantiation"
fi

# Find the start of Modules.cpp (all the includes)
# Add the include after the last top-level #include (before any #if guards)
LAST_PLAIN_INCLUDE=$(grep -n "^#include" "$MODULES_CPP" | tail -1 | cut -d: -f1)

if [ -n "$LAST_PLAIN_INCLUDE" ]; then
    # Find next non-include line
    INSERT_LINE=$((LAST_PLAIN_INCLUDE + 1))
    # But make sure we don't insert inside a #if block
    while [ $INSERT_LINE -le $(wc -l <"$MODULES_CPP") ]; do
        LINE=$(sed -n "${INSERT_LINE}p" "$MODULES_CPP")
        # If this line starts #if or #endif, go to next line
        if [[ "$LINE" =~ ^#(if|else|endif) ]]; then
            INSERT_LINE=$((INSERT_LINE + 1))
        else
            # Found a good place (empty line or start of comment)
            break
        fi
    done

    sed -i '' "${INSERT_LINE}i\\
#include \"BBSModule_v2.h\"
" "$MODULES_CPP"
    echo "✓ Added BBS include to Modules.cpp (before line $INSERT_LINE)"
fi

# Find the setupModules function and add instantiation after the opening brace
SETUP_MODULES_LINE=$(grep -n "void setupModules()" "$MODULES_CPP" | head -1 | cut -d: -f1)
if [ -n "$SETUP_MODULES_LINE" ]; then
    # Find the opening brace
    BRACE_LINE=$((SETUP_MODULES_LINE))
    while [ $BRACE_LINE -le $(wc -l <"$MODULES_CPP") ]; do
        LINE=$(sed -n "${BRACE_LINE}p" "$MODULES_CPP")
        if [[ "$LINE" == *"{"* ]]; then
            break
        fi
        BRACE_LINE=$((BRACE_LINE + 1))
    done

    if [ $BRACE_LINE -lt $(wc -l <"$MODULES_CPP") ]; then
        # Insert after the brace
        INSERTAFTER=$BRACE_LINE
        sed -i '' "${INSERTAFTER}a\\
    bbsModule = new BBSModule();
" "$MODULES_CPP"
        echo "✓ Added BBS module instantiation to setupModules()"
    fi
fi

echo ""
echo "=== Integration Complete ==="
echo "Next step: Run ./scripts/build.sh to compile the firmware"
