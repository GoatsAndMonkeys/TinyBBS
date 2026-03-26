# TinyBBS SideClique — Design Document

*"Your people. Your mesh. Always connected."*

A private, encrypted, peer-to-peer BBS where every node in a family or friend group is both client and server. Nodes sync asynchronously over LoRa — messages, status, locations, and commands flow between members even when they're never all online at the same time.

No internet. No central server. No single point of failure.

---

## How It Works

Every SideClique member runs the same firmware. They share an encrypted channel (PSK). When any two members are in range — even briefly — their nodes automatically sync all pending data. If Dad talks to Mom, and Mom later encounters Alex, Alex gets Dad's messages through Mom.

The geo city database from TinyBBS is reused — all locations show as city names (e.g., "Portland") instead of raw coordinates.

---

## Features

### 1. Check-In Board

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

### 2. Persistent DMs (Store & Forward)

Messages queue and retry until delivered — even days later:

```
You → Dad: "Picking up groceries, need anything?"
  ✓ Sent  ✓ Delivered  ○ Read

You → Sam: "Call home when you can"
  ✓ Sent  ○ Delivered (Sam offline 2 days)
  ↻ Will retry when any clique member sees Sam
```

**Retry logic:**
- Immediate when recipient is seen
- Every 5 min for first hour
- Every 30 min for first day
- Every 2 hours after that
- Expires after 7 days (configurable)
- ANY clique member can relay: if Mom has a DM for Dad, and Alex sees Dad first, Alex delivers it

**Delivery receipts**: Sent → Delivered → Read

### 3. SOS Emergency

**Self-triggered:**
```
You: !sos
```

**Parent/guardian-triggered (remote):**
```
Mom: !sos Alex
```

In both cases, the target node:
- Switches to GPS broadcast every 30 seconds
- Activates buzzer/LED
- All clique members see live location with city name
- Keeps retransmitting until ALL members ACK
- Cannot be silenced by the target when remotely triggered (parental override)
- Shows "SOS ACTIVE" on screen with sender info

All other clique nodes:
- Display SOS alert with location and city name
- Buzzer/LED alarm
- Auto-relay to any clique member they encounter
- Show live position updates as they arrive

### 4. Find My Family (LOCATE)

Any member can remotely activate GPS tracking on another member's node:

```
Mom: !locate Alex
→ Alex's node starts broadcasting GPS every 60 seconds
→ All clique nodes store and relay Alex's positions
→ Mom sees: "Alex: 45.51,-122.68 (Seattle) 2m ago, moving NE"
```

**Features:**
- City name from geo database
- Direction of travel (computed from position delta)
- Auto-stops after 4 hours
- Target node shows "LOCATE ACTIVE" on screen
- Target can cancel if self-triggered, but NOT if parent-triggered
- Optional: configurable to require 2 members to agree

### 5. Remote Ping

```
You: !ping Dad
→ "Dad: OK | 85% | Portland | Up:3h | LastMsg:5m ago"
```

Firmware responds automatically — Dad doesn't need to be actively using the node.

### 6. Remote Alert

```
Mom: !alert Alex "Come home for dinner"
→ Alex's node: buzzer sounds, LED flashes, message on screen
→ Doesn't clear until Alex ACKs
```

### 7. Shared Lists

```
=== Grocery List ===
☑ Water (Mom, 3/24)
☐ Batteries (Dad, 3/24)
☐ First aid kit (You, 3/25)

[A]dd [D]one [R]emove
```

Uses CRDT (Conflict-Free Replicated Data Type) — no conflicts when two people edit simultaneously.

### 8. Shared Bulletin Board

Group announcements that sync to all members:

```
=== Clique Board ===
#1 Dad: "Generator is running, house has power" (2h ago)
#2 Mom: "Roads clear on Route 26" (5h ago)
#3 Alex: "Cell towers down in Seattle" (1d ago)
```

### 9. Rally Points

Pre-set meeting locations shared across the clique:

```
=== Rally Points ===
1. Home         Portland
2. School       Portland
3. Uncle Bob's  Portland
4. Bridge       Portland

!rally 1 → "RALLY AT Home" broadcast to all members
```

### 10. Dead Drops

GPS-tagged messages for nearby members:

```
Dad: !drop "Left supplies under the bridge"
→ When any member is within 500m:
  "Dead drop from Dad (200m away): Left supplies under the bridge"
```

### 11. Canary / Wellness Check

Auto-alert when a member goes quiet:

```
⚠ Sam hasn't checked in for 24 hours
Last known: Portland (45.51,-122.68)
Battery was: 12%

!locate Sam to start tracking
!sos Sam to trigger emergency mode
```

Configurable per member: 6h for kids, 24h for adults, 48h for extended trips.

### 12. Battery Prediction

```
Alex: 45% ↓ (~2 hours remaining)
Sam: 12% ↓ CRITICAL (~20 minutes)
Dad: 85% → (charging)
```

Computed from battery drain rate over time.

---

## Sync Protocol

### Gossip with Vector Clocks

Each node maintains a sync vector — the highest message sequence number seen from each member:

```
My vector: {Dad:47, Mom:31, Alex:22, Me:55}
```

When two nodes meet:
1. Exchange sync vectors in BEACON
2. Compare: find what each side is missing
3. Delta sync: send only missing messages, one per LoRa packet
4. Trickle: ~1 message/minute during idle to avoid flooding

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

### Anti-Replay / Security

- All traffic on encrypted PSK channel (Meshtastic AES)
- Messages include timestamp — reject if >24h old
- Unique (sender, sequence) pair per message
- Bloom filter tracks seen message IDs
- Remote commands only accepted from known clique member node IDs

---

## Parental Controls

For family cliques with children:

| Feature | Child Node | Parent Node |
|---|---|---|
| SOS (remote) | Cannot cancel | Can trigger on child |
| LOCATE | Cannot cancel | Can trigger on child |
| ALERT | Must ACK to dismiss | Can send to child |
| Beacon interval | Can be set remotely | Controls child's interval |
| Status | Auto-reported | Sees all children |
| Canary timeout | Shorter (6h default) | Gets alert first |

**Implementation**: Each member has a `role` field — `ADMIN` (parent) or `MEMBER` (child/peer). Admins can send remote commands that members cannot override.

---

## Storage Layout (External Flash)

```
/clique/
├── config.bin          Member list, PSK hash, roles, settings
├── vector.bin          Sync vector (last seen seq per member)
├── members/
│   └── <nodeId>.bin    Status, position, battery, last seen
├── inbox/
│   └── <msgId>.bin     Received DMs
├── outbox/
│   └── <msgId>.bin     Pending DMs (retry until ACK)
├── board/
│   └── <seq>.bin       Shared bulletins
├── lists/
│   ├── grocery.bin     Shared list (CRDT)
│   └── supplies.bin
├── rally/
│   └── points.bin      Pre-set rally locations
├── drops/
│   └── <id>.bin        Dead drops
└── locate/
    └── <nodeId>.bin    Location breadcrumbs
```

**Estimated size for 10-person clique**: ~83 KB
**Available on 2MB flash**: ~1.4 MB after TinyBBS data

---

## Coexistence with TinyBBS

SideClique runs alongside TinyBBS on the same node:

- **TinyBBS**: Public BBS on the primary channel — anyone can use
- **SideClique**: Private family/friend mesh on an encrypted secondary channel
- Different storage paths (`/bbs/` vs `/clique/`)
- Different channels (public vs encrypted PSK)
- Same node, same radio, same firmware

A node could even be in multiple cliques (family + work team), each with its own PSK and storage namespace.

---

## Implementation Phases

### Phase 1: Core (MVP)
- Clique create/join (shared PSK)
- Member beacons (status + GPS + battery + city name)
- Check-in board (view all members)
- Persistent DMs with retry and delivery receipts
- Basic gossip sync with vector clocks

### Phase 2: Remote Commands
- SOS (self + remote trigger with parental override)
- LOCATE (remote GPS tracking)
- PING (remote status check)
- ALERT (remote buzzer/message)
- Canary / wellness check

### Phase 3: Collaboration
- Shared bulletin board
- Shared lists (CRDT)
- Rally points
- Dead drops

### Phase 4: Intelligence
- Location history / breadcrumbs
- Battery prediction
- Movement detection ("Alex is heading toward Rally Point 2")
- Geo-fencing alerts ("Sam left the Portland area")

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
