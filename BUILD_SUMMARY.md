# Meshtastic BBS Firmware Module - Build Summary

## ✅ Project Complete!

The BBS (Bulletin Board System) firmware module for Meshtastic has been successfully built for the Heltec LoRa 32 V3.

### Build Status
- **Target**: Heltec LoRa 32 V3 (ESP32-S3 with 8MB PSRAM)
- **Firmware Version**: v2.7.20.016e68e (latest stable)
- **Build Status**: ✅ SUCCESS
- **Build Time**: ~28 seconds (incremental)
- **Memory Usage**:
  - Flash: 63.0% (2.1 MB of 3.3 MB)
  - RAM: 40.3% (132 KB of 320 KB)
- **Binary Size**: 2.1 MB (+4KB for QSL feature)

### Generated Files

**Firmware binaries** (ready to flash):
- `firmware/.pio/build/heltec-v3/firmware-heltec-v3-2.7.20.016e68e.bin` (2.0 MB)
- `firmware/.pio/build/heltec-v3/firmware-heltec-v3-2.7.20.016e68e.factory.bin` (2.1 MB)

**Source files** (module-src/):
- `BBSStorage.h` - Abstract storage interface
- `BBSStoragePSRAM.h` - Fast in-memory PSRAM backend (500 bulletins, 128 mailboxes, 100 mail/user)
- `BBSStorageLittleFS.h` - Persistent flash-based backend (50 bulletins, 32 mailboxes, 20 mail/user)
- `BBSModule_v2.h` - Module header with command handlers
- `BBSModule_v2.cpp` - Module implementation

**Build/Deployment scripts** (scripts/):
- `integrate.sh` - Patches firmware and copies module files
- `build.sh` - Builds firmware using PlatformIO
- `flash.sh` - Flashes firmware to connected Heltec V3

### How to Use

#### Flash to Device
```bash
./scripts/flash.sh
```
This will:
1. Activate the Python venv
2. Flash the binary to your Heltec V3 (via USB)
3. Show serial monitor output

**Prerequisites**: Heltec V3 connected via USB-C cable

#### View Logs
```bash
cd firmware
source ~/.venv/bin/activate
pio device monitor -b 115200
```

### QSL Board Feature (New!)

The QSL board is a radio operator's "heard log" - it shows who you've received signals from and signal quality metrics:

**What Gets Posted**:
- **Station**: Node name and ID
- **Hops Away**: Number of relays between you and them (0 = direct, 1 = one relay, etc.)
- **Signal Quality**: SNR (Signal-to-Noise Ratio) 0-15, RSSI (signal strength)
- **Location** (optional): GPS coordinates if your node has GPS enabled
- **Timestamp**: When the QSL was posted

**How It Works**:
1. Node A sends any message to the mesh
2. Node B receives it and posts: `!bbs qsl post`
3. Board automatically captures: sender info, hop count, signal quality
4. All nodes can view with: `!bbs qsl list`

**Typical Use Cases**:
- "I heard Node-Alpha 5 hops away with good signal"
- Coverage mapping: Post your location and see who else is in range
- Signal quality survey: Compare SNR/RSSI across the network
- Repeater/relay testing: Track signal degradation through hops

**Storage**:
- PSRAM boards: 200 QSL posts (ephemeral, lost on reboot)
- Flash boards: 50 QSL posts (persistent, 24-hour auto-cleanup)
- Posts automatically expire after 24 hours to keep board fresh

### BBS Module Features

**Public Bulletins** (visible to all):
- `!bbs list [page]` - List bulletins (5 per page)
- `!bbs read <id>` - Read full bulletin
- `!bbs post <text>` - Post a bulletin (max 200 chars)
- `!bbs del <id>` - Delete your bulletin

**Private Mail** (person-to-person):
- `!bbs mail` - Check for unread messages
- `!bbs mail send <recipient> <text>` - Send mail
- `!bbs mail read <id>` - Read specific mail
- `!bbs mail del <id>` - Delete mail

**QSL Board** (radio confirmations):
- `!bbs qsl` or `!bbs qsl list` - Show recent QSL posts
- `!bbs qsl page N` - View page N of QSL board
- `!bbs qsl post` - Post your location & signal info
  - Automatically includes: hop count, SNR, RSSI
  - Optional: GPS coordinates if available

**Utilities**:
- `!bbs stats` - Show storage stats (includes QSL count)
- `!bbs help` - Show help

### Hardware Auto-Detection

The module automatically selects the best storage backend:

- **ESP32 with PSRAM** (e.g., Heltec V3): Uses fast in-memory PSRAM (data lost on reboot)
- **ESP32 without PSRAM**: Uses persistent LittleFS on flash
- **nRF52 boards** (e.g., RAK4631): Uses persistent LittleFS on flash

### Architecture

- **Modular Design**: Storage interface allows easy backend switching
- **Memory Efficient**: PSRAM backend uses ~3MB of 8MB available
- **Persistent**: LittleFS backend saves data across reboots
- **Rate Limiting**: Multi-part messages delayed 1.5s to avoid mesh flooding
- **Node Resolution**: Users can address mail by short name or hex ID

### Next Steps

1. **Flash the firmware** to your Heltec V3:
   ```bash
   ./scripts/flash.sh
   ```

2. **Configure a test node** (or use two Heltec boards)

3. **Test commands** via direct message:
   - Send: `!bbs help`
   - Post: `!bbs post Hello world`
   - List: `!bbs list`
   - Mail: `!bbs mail send NodeName Hey there!`

4. **Troubleshooting**:
   - View logs: `pio device monitor -b 115200`
   - BBS module logs with "BBS:" prefix
   - Storage backend selected on startup

### Rebuilding

If you modify the module source files:

```bash
./scripts/build.sh
```

This will:
1. Integrate modified files into firmware
2. Compile the firmware
3. Report binary location

### Technical Notes

- **Max message length**: 200 characters (fits in single mesh packet)
- **Reply max length**: 220 bytes (fits in mesh packet)
- **Multi-part messages**: Automatically split on newlines with 1.5s delays
- **Node addressing**: Can use short name, hex ID (0x12345678), or !hex format
- **Mesh packet port**: `TEXT_MESSAGE_APP` (port 1)
- **Command prefix**: `!bbs` (case-sensitive)

### File Locations

```
~/Documents/mesh_bbs/
├── firmware/               # Meshtastic firmware (patched)
├── module-src/             # BBS module source (unmodified)
├── scripts/                # Integration, build, flash scripts
├── tests/                  # Unit tests (planned)
├── docs/                   # Documentation
├── .venv/                  # Python venv with PlatformIO
└── BUILD_SUMMARY.md        # This file
```

---

**Built**: March 18, 2026
**Status**: Ready to flash and deploy! 🚀
