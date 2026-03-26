# TinyBBS — Meshtastic Bulletin Board System

A full-featured BBS module for [Meshtastic](https://meshtastic.org/) firmware, running on **nRF52840** devices (T-Echo, RAK4631, WisMesh Pocket, etc.) and **ESP32** boards (Heltec V3). Send a DM to the BBS node and get a TC2-style interactive menu system over LoRa mesh radio.

---

## Features

### TinyBBS (public BBS)

- **Bulletins** — post and read public messages, organized by board (General, Info, News, Urgent)
- **Private Mail** — person-to-person messaging by node name or ID
- **QSL Board** — signal confirmation log with SNR, RSSI, hops, GPS position
- **Wordle** — daily word game (2,000-word dictionary, bloom filter validation, shared with Vault-Tec hack)
- **Vault-Tec Hack** — Fallout-style terminal hacking minigame
- **Wasteland RPG** — Fallout-themed text RPG with combat, VATS targeting, shop, arena PvP, trainer fights, and Chairman Cheng final boss
- **Chess by Mail** — full chess with alpha-beta AI, board rendering, Elo ratings, LittleFS persistence
- **OLED/E-Ink Status Frame** — live BBS stats on the node's display
- **Hop-Count Tapback** — responds to "test"/"ack" in public channel with number emoji showing hops away
- **Ping Tapback** — responds to "ping" with 🏓

### SideClique (private family/friend mesh)

A peer-to-peer encrypted BBS that runs alongside TinyBBS on the same node. Each encrypted Meshtastic channel becomes a "clique" — a private sync group with its own members, DMs, and status board.

- **Check-in board** — member status (OK/HELP/TRAVELING/HOME/AWAY/SOS), battery, city name, last seen
- **Persistent DMs** — store-and-forward with delivery receipts; retry for up to 7 days
- **Inbox** — ring buffer of last 16 received DMs, viewable from the menu
- **SOS emergency** — self or remote trigger; broadcasts GPS every 30s, activates buzzer/LED
- **LOCATE** — remotely activate GPS tracking on any member's node
- **PING** — remote status query (responds automatically)
- **ALERT** — remote buzzer/message that must be acknowledged
- **Games** — Wordle, Vault-Tec Hack, Wasteland RPG, Daily Quest, Chess all accessible from SideClique menu
- **Persistence** — clique state and DM queue survive reboot via external QSPI flash
- **Auto-discovery** — detects all encrypted channels on startup, one clique per channel

See [docs/SIDECLIQUE.md](docs/SIDECLIQUE.md) for full protocol and feature documentation.

### nRF52 External Flash

On nRF52840 boards with QSPI flash (T-Echo, RAK4631, WisMesh Pocket, etc.), BBS data is stored on the **2MB external flash chip** — completely separate from Meshtastic's internal filesystem. This provides ~2MB of dedicated storage for bulletins, mail, QSL entries, game saves, and SideClique state. Falls back to internal LittleFS automatically on boards without QSPI.

### Session-Based Interface

DMs use a TC2-style menu state machine — no command prefix needed. Just DM the node and navigate with single-letter commands. Channel messages use `!bbs` prefix for one-shot commands.

---

## Supported Hardware

| Board | Status | Storage | Notes |
|---|---|---|---|
| LilyGO T-Echo (nRF52840) | ✅ Tested | 2MB external QSPI flash | Primary dev board |
| RAK4631 WisBlock (nRF52840) | ✅ Builds | 2MB external QSPI flash | ePaper display |
| RAKwireless WisMesh Pocket (nRF52840) | ✅ Builds | 2MB external QSPI flash | SideClique target |
| RAK4631 ePaper (nRF52840) | ✅ Builds | 2MB external QSPI flash | |
| Heltec Mesh Node T114 (nRF52840) | ✅ Builds | 2MB external QSPI flash | |
| Nano G2 Ultra (nRF52840) | ✅ Builds | 2MB external QSPI flash | |
| Tracker T1000-E (nRF52840) | ✅ Builds | Internal LittleFS | No QSPI flash |
| Heltec LoRa 32 V3 (ESP32-S3) | ✅ Builds | LittleFS / PSRAM | |
| Other nRF52840 boards | May work (build from source) | Internal LittleFS fallback | |
| Other ESP32 boards | May work (build from source) | LittleFS | |

> Only the T-Echo has been extensively tested on real hardware. If you test on another board, please open an issue and let us know!

---

## Quick Start: Flash Pre-Built Firmware

> **Easiest option** — no build environment needed.

Pre-built UF2 files are in `firmware-builds/`:

| File | Board |
|---|---|
| `TinyBBS-rak4631.uf2` | RAK4631 WisBlock |
| `TinyBBS-rak_wismeshtap.uf2` | WisMesh Pocket |

### nRF52840 boards (RAK4631, WisMesh Pocket, T-Echo)

1. Double-press the reset button → **RAK4631BOOT** / **TECHOBOOT** volume mounts
2. Copy the UF2 file:
   ```bash
   cp firmware-builds/TinyBBS-rak_wismeshtap.uf2 /Volumes/RAK4631BOOT/
   ```
3. Device reboots automatically

### Heltec V3 (ESP32-S3)

```bash
pip install esptool
esptool.py --chip esp32s3 --port /dev/cu.usbserial-0001 \
  --baud 921600 write_flash 0x0 firmware-heltec-v3.factory.bin
```

---

## Loading Data Files (External Flash)

The Wordle dictionary and geo city database must be loaded onto external flash. Use the **KB Loader** — a standalone firmware that turns the device into a serial-controlled flash programmer.

### Step 1: Flash the loader

```bash
cd loader
source ../.venv/bin/activate
pio run -e kb-loader-rak   # for RAK4631 / WisMesh Pocket
pio run -e kb-loader       # for T-Echo
```

Flash the resulting UF2 from `.pio/build/kb-loader-rak/firmware.uf2` via double-reset.

### Step 2: Generate data files

```bash
source .venv/bin/activate
python3 scripts/gen_wordle.py       # creates module-src/BBSWordleData.h (2,000 words)
python3 scripts/gen_wordle_packed.py  # creates data/wordle.bin
python3 scripts/gen_geo_packed.py     # creates data/geo_us.bin (370KB, 16,726 US cities)
```

### Step 3: Upload over USB serial

```bash
pip install pyserial
python3 scripts/upload_serial.py data/wordle.bin /bbs/kb/wordle.bin
python3 scripts/upload_serial.py data/geo_us.bin /bbs/kb/geo_us.bin
```

Auto-detects the serial port. Override with `--port /dev/cu.usbmodemXXXX`.

### Step 4: Flash the real firmware

After upload completes, flash TinyBBS firmware normally (double-reset + copy UF2).

### Verify upload

DM the BBS node and send `S` for stats. You should see `[ExtFlash]` with reduced free space.

---

## Build From Source

### Prerequisites

- Python 3.12+ with pip
- PlatformIO (installed via pip)
- Git

### Setup

```bash
git clone https://github.com/GoatsAndMonkeys/TinyBBS.git
cd TinyBBS

# Clone Meshtastic firmware (if not already)
git clone --recurse-submodules https://github.com/meshtastic/firmware.git

python3 -m venv .venv
source .venv/bin/activate
pip install platformio
```

### Build

```bash
./scripts/integrate.sh   # copies BBS + SideClique files into firmware tree

cd firmware
source ../.venv/bin/activate

pio run -e rak_wismeshtap   # WisMesh Pocket (SideClique primary target)
pio run -e rak4631          # RAK4631 WisBlock
```

### Flash

Double-press reset on the device, then copy the UF2:

```bash
cp .pio/build/rak_wismeshtap/firmware.uf2 /Volumes/RAK4631BOOT/
```

---

## Usage

### Via Direct Message (TinyBBS)

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

Navigate with single-letter commands.

### Via Direct Message (SideClique)

If SideClique is enabled and the node has at least one encrypted channel configured:

```
SideClique [1 cliques 4 members]
[C]heck-in board
[I]nbox (2)
[D]M send
[S]tatus update
[P]ing member
[!]SOS
[R]Wastelad RPG
[Q]Daily Quest
[H]ack Terminal
[W]ordle
[K]Chess by Mesh
[X]Exit
```

> SideClique auto-activates when the device has a PSK set on any secondary channel.

### Via Public Channel (TinyBBS)

Prefix commands with `!bbs`:

| Command | Action |
|---|---|
| `!bbs` | Quick help |
| `!bbs list` | Recent bulletins |
| `!bbs post <text>` | Post a bulletin |
| `!bbs stats` | Node stats |
| `!bbs qsl` | Log a QSL entry |

### Games

| Game | TinyBBS | SideClique | Description |
|---|---|---|---|
| **Wordle** | ✅ | ✅ | Daily 5-letter word game, 6 guesses, 2,000-word list |
| **Vault-Tec Hack** | ✅ | ✅ | Fallout terminal hacking — guess the password |
| **Wasteland RPG** | ✅ | ✅ | Full RPG: explore, fight, loot, level up |
| **Chess** | ✅ | ✅ | Chess by mail with AI opponent |
| **Daily Quest** | ❌ | ✅ | SideClique-exclusive survival quest |

---

## File Structure

```
TinyBBS/
├── module-src/                    # BBS module source files
│   ├── BBSModule_v2.h/.cpp        # Main module: menus, bulletins, mail, QSL, games
│   ├── BBSStorage.h               # Abstract storage interface
│   ├── BBSStorageLittleFS.h       # LittleFS backend (ESP32 + nRF52 fallback)
│   ├── BBSStoragePSRAM.h          # PSRAM backend (ESP32 with PSRAM)
│   ├── BBSStorageExtFlash.h       # External QSPI flash backend (nRF52840)
│   ├── BBSExtFlash.h/.cpp         # QSPI flash LittleFS driver
│   ├── BBSWordle.h                # Wordle game + bloom filter validation
│   ├── BBSWordleData.h            # Generated: 2,000-word dict + 3KB bloom filter
│   ├── BBSChess.h/.cpp            # Chess engine + AI
│   ├── FalloutWastelandRPG.h/.cpp # Wasteland RPG
│   └── SCDailyQuest.h             # SideClique Daily Quest
├── firmware/src/modules/
│   ├── SideClique.h               # SideClique module header
│   └── SideClique.cpp             # SideClique: beacons, DMs, sync, games, persistence
├── loader/                        # Standalone serial flash loader firmware
│   ├── src/main.cpp               # 0xBB-framed serial protocol, QSPI flash writer
│   └── platformio.ini             # kb-loader (T-Echo), kb-loader-rak (RAK4631/WisMesh)
├── scripts/
│   ├── integrate.sh               # Patch module into firmware tree
│   ├── gen_wordle.py              # Generate BBSWordleData.h (2,000 words + bloom filter)
│   ├── gen_wordle_packed.py       # Generate data/wordle.bin for external flash
│   ├── gen_geo_packed.py          # Generate data/geo_us.bin city database
│   ├── upload_serial.py           # Upload files to device over USB serial
│   └── ble_upload.py              # Upload files over BLE (slow, wireless)
├── firmware-builds/               # Pre-built UF2 firmware files
│   ├── TinyBBS-rak4631.uf2
│   └── TinyBBS-rak_wismeshtap.uf2
└── docs/
    └── SIDECLIQUE.md              # SideClique protocol and feature reference
```

---

## Build Stats

### WisMesh Pocket / RAK4631 (nRF52840)

| Build | Flash | RAM |
|---|---|---|
| `rak_wismeshtap` (WisMesh Pocket) | 94.1% (767KB / 815KB) | 33.4% (83KB / 249KB) |
| `rak4631` (RAK4631 WisBlock) | 96.2% (784KB / 815KB) | 34.5% (86KB / 249KB) |
| `kb-loader-rak` (serial loader) | 10.7% (87KB / 815KB) | 3.9% (10KB / 249KB) |

### BBS Module Sizes (approximate)

| Module | Flash | What it contains |
|---|---|---|
| SideClique.cpp | ~45KB | Peer BBS: beacons, DMs, sync, games, persistence |
| BBSModule_v2.cpp | ~38KB | Core BBS: menus, bulletins, mail, QSL, state machine |
| FalloutWastelandRPG.cpp | ~21KB | Wasteland RPG: combat, VATS, inventory, 16 enemies |
| BBSChess.cpp | ~9KB | Chess: move gen, alpha-beta AI, board rendering, Elo |
| BBSExtFlash.cpp | ~3KB | QSPI external flash driver + LittleFS |

---

## Technical Notes

- **Message limit**: 220 bytes per reply (Meshtastic TEXT_MESSAGE_APP supports 234 bytes)
- **nRF52 storage**: 2MB external QSPI flash via custom LittleFS driver, separate from Meshtastic's internal FS
- **ESP32 storage**: LittleFS on flash, PSRAM if available (>1MB free)
- **SideClique storage**: `/clique/state.bin` (member state) + `/clique/dmq/` (pending DMs)
- **Wordle**: 2,000-word curated list + 3,072-byte bloom filter (8 FNV-1a hashes, ~0.27% false positive rate)
- **Meshtastic version**: built against v2.7.x
- **SideClique port**: PRIVATE_APP (256) — invisible to Meshtastic app
- **SideClique DMs**: one-hop routed, with relay queue for offline members (Phase 2)

---

## License

MIT
