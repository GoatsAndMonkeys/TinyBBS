# TinyBBS — Meshtastic Bulletin Board System

A full-featured BBS module for [Meshtastic](https://meshtastic.org/) firmware, running on **nRF52840** devices (T-Echo, RAK4631, etc.) and **ESP32** boards (Heltec V3). Send a DM to the BBS node and get a TC2-style interactive menu system over LoRa mesh radio.

---

## Features

- **Bulletins** — post and read public messages, organized by board (General, Info, News, Urgent)
- **Private Mail** — person-to-person messaging by node name or ID
- **QSL Board** — signal confirmation log with SNR, RSSI, hops, GPS position
- **Wordle** — daily word game with score tracking
- **Vault-Tec Hack** — Fallout-style terminal hacking minigame
- **Wasteland RPG** — Fallout-themed text RPG with combat, VATS targeting, shop, arena PvP, trainer fights, and Chairman Cheng final boss
- **Chess by Mail** — full chess with alpha-beta AI, board rendering, Elo ratings, LittleFS persistence
- **OLED/E-Ink Status Frame** — live BBS stats on the node's display
- **Hop-Count Tapback** — responds to "test"/"ack" in public channel with number emoji showing hops away
- **Ping Tapback** — responds to "ping" with 🏓

### nRF52 External Flash

On nRF52840 boards with QSPI flash (T-Echo, RAK4631, etc.), BBS data is stored on the **2MB external flash chip** — completely separate from Meshtastic's internal filesystem. This provides ~2MB of dedicated storage for bulletins, mail, QSL entries, and game saves. Falls back to internal LittleFS automatically on boards without QSPI.

### Session-Based Interface

DMs use a TC2-style menu state machine — no command prefix needed. Just DM the node and navigate with single-letter commands. Channel messages use `!bbs` prefix for one-shot commands.

---

## Supported Hardware

| Board | Status | Storage |
|---|---|---|
| LilyGO T-Echo (nRF52840) | ✅ Tested | 2MB external QSPI flash |
| RAK4631 WisBlock (nRF52840) | ⚠️ Untested (firmware provided) | 2MB external QSPI flash |
| RAK4631 ePaper (nRF52840) | ⚠️ Untested (firmware provided) | 2MB external QSPI flash |
| Heltec Mesh Node T114 (nRF52840) | ⚠️ Untested (firmware provided) | 2MB external QSPI flash |
| Nano G2 Ultra (nRF52840) | ⚠️ Untested (firmware provided) | 2MB external QSPI flash |
| Tracker T1000-E (nRF52840) | ⚠️ Untested (firmware provided) | Internal LittleFS |
| Heltec LoRa 32 V3 (ESP32-S3) | ⚠️ Untested (firmware provided) | LittleFS / PSRAM |
| Other nRF52840 boards | May work (build from source) | Internal LittleFS fallback |
| Other ESP32 boards | May work (build from source) | LittleFS |

> **Note:** Only the T-Echo has been tested on real hardware. Firmware for other boards compiles but has not been verified. If you test on another board, please open an issue and let us know!

---

## Quick Start: Flash Pre-Built Firmware

> **Easiest option** — no build environment needed.

Download the latest firmware from the [Releases](https://github.com/GoatsAndMonkeys/TinyBBS/releases) page.

### T-Echo (nRF52840)

1. Double-press the reset button on T-Echo → **TECHOBOOT** volume mounts
2. Copy the UF2 file:
   ```bash
   cp TinyBBS-v0.0.260323-alpha-t-echo.uf2 /Volumes/TECHOBOOT/
   ```
3. Device reboots automatically (ignore macOS fcopyfile errors — they're harmless)

### Heltec V3 (ESP32-S3)

**Option A: Web Flasher**
1. Go to [https://flasher.meshtastic.org](https://flasher.meshtastic.org)
2. Connect your Heltec V3 via USB
3. Select **Custom Firmware** and upload the `.factory.bin` file
4. Click Flash

**Option B: esptool**
```bash
pip install esptool
esptool.py --chip esp32s3 --port /dev/cu.usbserial-0001 \
  --baud 921600 write_flash 0x0 firmware-heltec-v3.factory.bin
```

---

## Build From Source

### Prerequisites

- [Python 3.12+](https://python.org) with pip
- [PlatformIO](https://platformio.org/) (installed via pip)
- Git

### 1. Clone this repo

```bash
git clone https://github.com/GoatsAndMonkeys/TinyBBS.git
cd TinyBBS
```

### 2. Clone Meshtastic firmware

```bash
git clone --recurse-submodules https://github.com/meshtastic/firmware.git
```

### 3. Set up Python environment

```bash
python3 -m venv .venv312
source .venv312/bin/activate   # Windows: .venv312\Scripts\activate
pip install platformio
```

### 4. Integrate and build

```bash
./scripts/integrate.sh   # copies BBS module files into firmware tree

# Build for T-Echo (nRF52840)
cd firmware
source ../.venv312/bin/activate
pio run -e t-echo

# Build for other boards
pio run -e rak4631
pio run -e heltec-v3
```

### 5. Flash

**T-Echo:** Double-press reset, then copy the UF2:
```bash
cp .pio/build/t-echo/firmware.uf2 /Volumes/TECHOBOOT/
```

**Heltec V3:**
```bash
pio run -e heltec-v3 -t upload --upload-port /dev/cu.usbserial-0001
```

---

## Usage

### Via Direct Message

DM the BBS node from the Meshtastic app. You'll get the main menu:

```
TinyBBS nRF52
[B]ulletins
[M]ail
[Q]SL
[G]ames
[S]tats
[X]Exit
```

Navigate with single-letter commands. Type `H` at any time for help.

### Via Public Channel

Prefix commands with `!bbs`:

| Command | Action |
|---|---|
| `!bbs` | Quick help |
| `!bbs list` | Recent bulletins |
| `!bbs post <text>` | Post a bulletin |
| `!bbs stats` | Node stats |
| `!bbs qsl` | Log a QSL entry (also announces to channel) |

### Games Menu

| Game | Description |
|---|---|
| **[W]ordle** | Daily 5-letter word game, 6 guesses |
| **[V]ault-Tec Hack** | Fallout terminal hacking — guess the password |
| **[R]PG Wasteland** | Full RPG: explore, fight, loot, level up |
| **[C]hess** | Chess by mail with AI opponent |

### Wasteland RPG Commands

| Command | Action |
|---|---|
| `EX` | Explore the wasteland (costs 1 AP) |
| `A` | Attack in combat |
| `V` | VATS targeting system |
| `D` | Defend |
| `F` | Flee |
| `S` | Use Stimpak |
| `DR` | Visit the Doc (free daily heal) |
| `TR` | Train / level up |
| `SH` | Shop (weapons, armor, stimpaks) |
| `AR <name>` | Arena PvP (costs 3 AP) |
| `LB` | Leaderboard |
| `CH` | Fight Chairman Cheng (final boss, level 12+) |
| `H` | Help |
| `X` | Exit back to BBS |

---

## File Structure

```
TinyBBS/
├── module-src/                    # BBS module source files
│   ├── BBSModule_v2.h/.cpp        # Main module: menus, state machine, all features
│   ├── BBSStorage.h               # Abstract storage interface
│   ├── BBSStorageLittleFS.h       # LittleFS backend (ESP32 + nRF52 fallback)
│   ├── BBSStoragePSRAM.h          # PSRAM backend (ESP32 with PSRAM)
│   ├── BBSStorageExtFlash.h       # External QSPI flash backend (nRF52840)
│   ├── BBSExtFlash.h/.cpp         # QSPI flash LittleFS driver
│   ├── BBSPlatform.h              # Cross-platform macros
│   ├── BBSWordle.h                # Wordle game + dictionary
│   ├── BBSChess.h/.cpp            # Chess engine + AI
│   ├── FalloutWastelandRPG.h/.cpp # Wasteland RPG
│   ├── MudGame.h/.cpp             # TinyMUD (included, not active)
│   └── MudWorld.h                 # MUD world data
├── scripts/
│   ├── integrate.sh               # Patch module into firmware tree
│   ├── build.sh                   # PlatformIO build
│   ├── flash.sh                   # Flash to device
│   ├── gen_geo.py                 # Generate city lookup grid data
│   ├── gen_bloom.py               # Generate Wordle bloom filter
│   └── migrate_bbs.py             # Data migration utilities
└── docs/                          # Additional documentation
```

---

## Build Stats (T-Echo nRF52840)

- **Flash:** 98.2% (800KB / 815KB)
- **RAM:** 30.1% (75KB / 249KB)
- **External storage:** 2MB QSPI flash for BBS data

---

## Technical Notes

- **Message limit**: 200 bytes per Meshtastic packet (multi-part messages split automatically)
- **nRF52 storage**: 2MB external QSPI flash via custom LittleFS driver, separate from Meshtastic's internal FS
- **ESP32 storage**: LittleFS on flash, PSRAM if available (>1MB free)
- **RPG saves**: stored at `/bbs/frpg/p<nodenum>.bin` on device flash
- **Meshtastic version**: built against v2.7.x

---

## License

MIT
