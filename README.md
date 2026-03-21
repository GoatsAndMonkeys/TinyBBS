# TinyBBS — Meshtastic Bulletin Board System

A full-featured BBS module for [Meshtastic](https://meshtastic.org/) firmware, built for the **Heltec LoRa 32 V3** (ESP32-S3). Send a DM to the BBS node and get a TC2-style interactive menu system over LoRa mesh radio.

---

## Features

- **Bulletins** — post and read public messages, organized by board
- **Private Mail** — person-to-person messaging by node name or ID
- **QSL Board** — ham radio-style signal confirmation log with SNR/RSSI/hops
- **Wordle** — daily word game with leaderboard announcements
- **Wastelad RPG** — Fallout-themed text RPG with combat, VATS targeting, hacking minigame, shop, arena PvP, trainer fights, and Chairman Cheng final boss
- **Casino** — blackjack, roulette, and slots (play money or real in-game caps)
- **Weather Forecast** — daily weather pulled via Open-Meteo API (requires WiFi)
- **OLED Status Frame** — live BBS stats on the node's display
- **Ping Tapback** — responds to "ping" in public channel with 🏓

### Session-Based Interface
DMs use a TC2-style menu state machine — no command prefix needed. Just DM the node and navigate with single-letter commands. Channel messages use `!bbs` prefix for one-shot commands.

---

## Supported Hardware

| Board | Status |
|---|---|
| Heltec LoRa 32 V3 (ESP32-S3) | ✅ Primary target |
| Other ESP32 Meshtastic boards | Should work (untested) |

---

## Quick Start: Flash Pre-Built Firmware

> **Easiest option** — no build environment needed.

Download the latest `firmware-heltec-v3.factory.bin` from the [Releases](https://github.com/GoatsAndMonkeys/TinyBBS/releases) page.

### Option A: Web Flasher (easiest)

1. Go to [https://flasher.meshtastic.org](https://flasher.meshtastic.org)
2. Connect your Heltec V3 via USB
3. Select **Custom Firmware** and upload the `.factory.bin` file
4. Click Flash

### Option B: esptool (command line)

```bash
pip install esptool

esptool.py --chip esp32s3 --port /dev/cu.usbserial-0001 \
  --baud 921600 write_flash 0x0 firmware-heltec-v3.factory.bin
```

Replace `/dev/cu.usbserial-0001` with your actual port (`COM3` on Windows, `/dev/ttyUSB0` on Linux).

---

## Build From Source

### Prerequisites

- [Python 3.x](https://python.org) with pip
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
mv firmware firmware-upstream
```

> The build scripts expect the firmware at `../firmware` relative to the repo root. Adjust `scripts/integrate.sh` if your layout differs.

### 3. Set up Python environment

```bash
python3 -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install platformio
```

### 4. Integrate, build, and flash

```bash
./scripts/integrate.sh   # copies module files into firmware tree
./scripts/build.sh       # compiles firmware (~25 seconds)
./scripts/flash.sh       # flashes to connected Heltec V3
```

If `flash.sh` fails with a port detection error, flash manually:

```bash
source .venv/bin/activate
cd firmware
pio run -e heltec-v3 -t upload --upload-port /dev/cu.usbserial-0001
```

### 5. Monitor serial output

```bash
cd firmware
source ../.venv/bin/activate
pio device monitor -b 115200
```

---

## Usage

### Via Direct Message

DM the BBS node from the Meshtastic app. You'll get the main menu:

```
TinyBBS: VaultTec Ed.
[B]ulletins [M]ail
[Q]SL [G]ames [S]tats
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
| `!bbs qsl` | View QSL board |

### Wastelad RPG Commands

Once in the RPG (Games → RPG), use these commands:

| Command | Action |
|---|---|
| `EX` | Explore the wasteland (costs 1 AP) |
| `A` | Attack in combat |
| `V` | VATS targeting system |
| `D` | Defend |
| `F` | Flee |
| `S` | Use Stimpak |
| `DR` | Visit the Doc (free daily heal) |
| `TR` | Train / level up (when XP threshold met) |
| `SH` | Shop (weapons, armor, stimpaks) |
| `AR <name>` | Arena PvP (costs 3 AP) |
| `LB` | Leaderboard |
| `CH` | Fight Chairman Cheng (final boss, level 12+) |
| `TV` | Tavern (NPC quotes) |
| `H` | Help / command reference |
| `X` | Exit back to BBS |

---

## File Structure

```
TinyBBS/
├── module-src/                 # BBS module source files
│   ├── BBSModule_v2.h/.cpp     # Main BBS module
│   ├── BBSStorage.h            # Storage interface
│   ├── BBSStorageLittleFS.h    # LittleFS backend (persistent)
│   ├── BBSStoragePSRAM.h       # PSRAM backend (fast, volatile)
│   ├── BBSWordle.h             # Wordle game
│   ├── FalloutWastelandRPG.h/.cpp  # Wastelad RPG
│   └── BBSChess.h/.cpp         # Chess (in development)
├── scripts/
│   ├── integrate.sh            # Patch module into firmware tree
│   ├── build.sh                # PlatformIO build
│   └── flash.sh                # Flash to device
└── docs/                       # Additional documentation
```

---

## Technical Notes

- **Message limit**: 200 bytes per Meshtastic packet (multi-part messages split automatically)
- **Storage**: LittleFS on flash for persistent data (bulletins, mail, RPG saves)
- **RPG saves**: stored at `/bbs/frpg/p<nodenum>.bin` on device flash
- **Daily resets**: midnight Eastern Time (UTC-4/UTC-5)
- **Meshtastic version**: built against v2.7.20

---

## License

MIT
