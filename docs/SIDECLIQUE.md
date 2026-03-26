# TinyBBS SideClique — Design Document

*"Your people. Your mesh. Always connected."*

A private, encrypted, peer-to-peer BBS where every node in a family or friend group is both client and server. Nodes sync asynchronously over LoRa — messages, status, locations, and commands flow between members even when they're never all online at the same time.

No internet. No central server. No single point of failure.

---

## Implementation Status

| Feature | Status | Notes |
|---|---|---|
| Clique auto-discovery | ✅ Done | Scans all encrypted channels on first message |
| Member beacons (GPS, battery, city) | ✅ Done | Every 15 min; increments per-clique seq counter |
| Check-in board | ✅ Done | Status, battery %, city name, time since last seen |
| Persistent DMs + retry | ✅ Done | Store-and-forward, retry schedule, delivery ACK |
| DM inbox | ✅ Done | 16-slot ring buffer, newest-first display |
| Persistence (state + DM queue) | ✅ Done | `/clique/state.bin` + `/clique/dmq/` on ext flash |
| SOS (self + remote) | ✅ Done | GPS every 30s; remote triggers target node |
| LOCATE | ✅ Done | Remote GPS tracking at 60s interval |
| PING | ✅ Done | Returns status, battery, location, uptime |
| ALERT | ✅ Done | Remote buzzer/message |
| Battery reading | ✅ Done | `powerStatus->getBatteryChargePercent()` |
| Seq counter (per clique) | ✅ Done | `cliques_[c].mySeq++` on beacon |
| Games (Wordle, Chess, Hack, RPG, Quest) | ✅ Done | All accessible from SideClique menu |
| drawFrame (screen display) | ✅ Done | Clique count, member count, SOS/LOCATE indicator |
| Gossip sync (vector clocks) | 🔲 Phase 2 | Beacon carries one seq; full multi-vector Phase 4 |
| handleSyncReq / handleSyncData | 🔲 Phase 2 | Stubs only |
| DM relay queue | 🔲 Phase 2 | Offline-target relay not yet implemented |
| Bulletin board menu | 🔲 Phase 3 | `SC_STATE_BOARD` wired, `sendBulletinBoard()` stub |
| Shared lists (CRDT) | 🔲 Phase 3 | Not started |
| Rally points | 🔲 Phase 3 | Not started |
| Dead drops | 🔲 Phase 3 | Not started |
| Buzzer/LED for SOS + ALERT | 🔲 Phase 2 | NCP5623 RGB LED lib available in rak_wismeshtap |
| Battery prediction | 🔲 Phase 4 | Needs drain rate tracking |
| Geo-fencing | 🔲 Phase 4 | Not started |

---

## How It Works

Every SideClique member runs the same firmware. They share an encrypted channel (PSK). When any two members are in range — even briefly — their nodes automatically sync all pending data.

The geo city database from TinyBBS is reused — all locations show as city names (e.g., "Portland") instead of raw coordinates.

SideClique auto-discovers cliques by scanning all Meshtastic channels on first startup. Any channel with a non-default PSK becomes a clique. Clique traffic uses port 256 (PRIVATE_APP) — invisible to the Meshtastic app. Human chat still uses port 1 (TEXT_MESSAGE_APP) and appears normally in the app.

---

## Menu

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

---

## Features (implemented)

### Check-In Board

Every member has a status visible to the whole clique:

```
=== SideClique ===
Dad    OK    2m ago  85%  Portland
Mom    OK    15m ago 72%  Portland
Alex   TRVL  3h ago  45%  Seattle
Sam    ??    2d ago  --   last: Portland
```

**Statuses**: OK, HELP, TRAVELING, HOME, AWAY, SOS, ?? (auto after 24h no beacon)

Auto-beacon every 15 min with GPS + battery + city name.

### DM Inbox

Received DMs are stored in a 16-slot ring buffer. The menu shows the count of stored messages. `[I]nbox` shows the 3 most recent DMs with sender name and message text.

### Persistent DMs (Store & Forward)

Messages queue and retry until delivered:

```
You → Dad: "Picking up groceries, need anything?"
  ✓ Sent  ✓ Delivered  ○ Read

You → Sam: "Call home when you can"
  ✓ Sent  ○ Delivered (Sam offline 2 days)
  ↻ Will retry when any clique member sees Sam
```

Retry schedule: immediate → every 5 min (first hour) → every 30 min (first day) → every 2 hours. Expires after 7 days.

Pending DMs survive reboot: stored at `/clique/dmq/<id_hex>.bin`.

### SOS Emergency

**Self-triggered:**
```
You: !sos
```

**Parent/guardian-triggered (remote):**
```
Mom: !sos Alex
```

Target node broadcasts GPS every 30 seconds. All clique members see live location.

### LOCATE

```
Mom: !locate Alex  (or: !f Alex from menu)
→ Alex's node starts broadcasting GPS every 60 seconds
```

### PING

```
You: !ping Dad  (or: [P] from menu)
→ "Dad: OK | 85% | Portland | Up | Last: 5m ago"
```

Firmware responds automatically.

### ALERT

```
!alert Alex "Come home for dinner"
→ Alex's node: message displayed
```

---

## Sync Protocol

### Current (implemented)

Each beacon includes the sender's per-clique sequence counter (`mySeq`). Recipients update their view of that member's highest known sequence. This is the foundation for future gossip sync.

### Phase 2 (planned)

Each node maintains a sync vector — the highest message sequence number seen from each member:

```
My vector: {Dad:47, Mom:31, Alex:22, Me:55}
```

When two nodes meet:
1. Exchange sync vectors in BEACON
2. Compare: find what each side is missing
3. Delta sync: send only missing messages

### Message Types

| Type | Code | Priority | Purpose |
|---|---|---|---|
| SOS | 0xFF | Highest | Emergency — all relay immediately |
| ALERT | 0x52 | High | Remote buzzer/message |
| LOCATE | 0x50 | High | Remote GPS tracking |
| DM | 0x10 | Normal | Private message |
| DM_ACK | 0x11 | Normal | Delivery receipt |
| DM_READ | 0x12 | Low | Read receipt |
| STATUS | 0x20 | Normal | Check-in update |
| BULLETIN | 0x30 | Normal | Shared post |
| LIST_OP | 0x40 | Low | Shared list change |
| BEACON | 0x01 | Background | Periodic heartbeat |
| SYNC_REQ | 0x02 | Background | Request missing data |
| SYNC_DATA | 0x03 | Background | Deliver missing data |
| PING | 0x51 | Normal | Remote status request |

---

## Storage Layout (External Flash — implemented)

```
/clique/
├── state.bin          Clique list, member data, sync vectors (magic 0x53435101)
└── dmq/
    └── <id_hex>.bin   One file per pending DM (SCPendingDM struct)
```

**State file format** (binary):
```
magic:4  version:1  clique_count:1
per clique:
  channelIndex:1  memberCount:1  mySeq:4  name:16
  per member:
    SCMember struct (nodeNum, name, role, status, lat, lon, alt, battery, lastSeen, syncSeq, location, flags)
```

**DM queue file format**: raw `SCPendingDM` struct per file. Files are auto-deleted when the DM is delivered or expires.

### Planned (Phase 3)

```
/clique/
├── board/
│   └── <seq>.bin      Shared bulletin posts
├── lists/
│   └── <name>.bin     Shared lists (CRDT)
└── rally/
    └── points.bin     Pre-set rally locations
```

**Estimated size for 10-person clique**: ~83 KB
**Available on 2MB flash**: ~1.4 MB after TinyBBS data

---

## Parental Controls

For family cliques with children:

| Feature | Child Node | Parent Node |
|---|---|---|
| SOS (remote) | Cannot cancel | Can trigger on child |
| LOCATE | Cannot cancel | Can trigger on child |
| ALERT | Must ACK to dismiss | Can send to child |
| Canary timeout | Shorter (6h default) | Gets alert first |

**Implementation**: Each member has a `role` field — `ADMIN` (parent) or `MEMBER` (child/peer). Admins can send remote commands that members cannot override.

---

## Coexistence with TinyBBS

SideClique runs alongside TinyBBS on the same node:

- **TinyBBS**: Public BBS on the primary channel — anyone can use
- **SideClique**: Private family/friend mesh on encrypted secondary channel(s)
- Different storage paths (`/bbs/` vs `/clique/`)
- Different channels (public vs encrypted PSK)
- Same node, same radio, same firmware

A node can be in multiple cliques (family + work team), each with its own PSK channel and storage namespace.

---

## Bandwidth Budget

LoRa LongFast ≈ 1 message every 30 seconds (shared with all mesh traffic).

| Activity | Frequency | Packets/hr |
|---|---|---|
| Beacon (per member) | Every 15 min | 4 |
| Sync (background) | 1/min when peer seen | 60 |
| DM send | On demand | 1-5 |
| DM retry | 5min/30min/2hr | 1-12 |
| LOCATE updates | Every 60s when active | 60 |
| SOS broadcasts | Every 30s when active | 120 |

A 5-person clique in normal mode: ~30 packets/hour (very light).
During SOS/LOCATE: ~120-180 packets/hour (heavy but justified).

---

## Target Hardware

Primary development target: **RAKwireless WisMesh Pocket** (`rak_wismeshtap`)

- nRF52840 SoC
- 2MB external QSPI flash (P0.03/P0.26/P0.30/P0.29/P0.28/P0.02)
- NCP5623 RGB LED (available for SOS/ALERT buzzer — Phase 2)
- LoRa SX1262 radio
- E-paper or OLED display

Also tested on: **LilyGO T-Echo** (`t-echo`), **RAK4631 WisBlock** (`rak4631`)
