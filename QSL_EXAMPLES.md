# QSL Board Examples

## What is QSL?

QSL stands for "Confirmation of Receipt" in amateur radio. In this Meshtastic BBS context, it's a public board where mesh operators can log which stations they've heard and at what signal quality.

Think of it as a "heard log" for your mesh network - useful for:
- Confirming connectivity across the network
- Documenting signal quality between nodes
- Coverage mapping
- Troubleshooting reception issues

## Command Examples

### 1. View Recent QSL Posts

```
User sends: !bbs qsl list
Bot replies:
=== QSL Board Page 1 ===
[1] Alice (2 hops) - has location
[2] Charlie (1 hop)
[3] Bob (0 hops)
[4] Eve (3 hops) - has location
[5] Dave (1 hop)
Use !bbs qsl post to add your location
```

### 2. View Next Page

```
User sends: !bbs qsl page 2
Bot replies:
=== QSL Board Page 2 ===
[6] Frank (2 hops)
[7] Grace (0 hops)
[8] Hank (4 hops) - has location
Use !bbs qsl post to add your location
```

### 3. Post Your Location

```
User sends: !bbs qsl post
Bot replies: Posted QSL #9 - 1 hops away, SNR: 8
```

The post automatically includes:
- Your node name (e.g., "MyNode")
- Who's hearing you (the sender of the command)
- How many relays between you
- Signal quality (SNR value, higher = better)
- Your GPS location (if available)

### 4. Check Storage Stats

```
User sends: !bbs stats
Bot replies:
BBS Stats:
Bulletins: 12/500
Mail: 3 items
QSL: 18 posts
Free: 2156 bytes
```

## Use Cases

### Network Coverage Testing

1. Node A posts: `!bbs qsl post`
   - Creates entry: "NodeA (2 hops)"
2. Node B posts: `!bbs qsl post`
   - Creates entry: "NodeB (0 hops)" (direct connection)
3. Node C posts: `!bbs qsl post`
   - Creates entry: "NodeC (1 hop)" (relayed through NodeB)

Result: Network operators can see the topology and signal quality at a glance.

### Signal Quality Survey

Operators in the same area post QSL to compare SNR values:

```
User_1: !bbs qsl post
→ Posted QSL #15 - 0 hops away, SNR: 14

User_2: !bbs qsl post
→ Posted QSL #16 - 0 hops away, SNR: 10

User_3: !bbs qsl post
→ Posted QSL #17 - 0 hops away, SNR: 12
```

Viewing the board shows signal quality differences between antennas/locations.

### Repeater/Relay Testing

Testing a new repeater or relay node:

```
NodeA (primary): !bbs qsl post
→ Posted QSL #20 - 0 hops away, SNR: 15

NodeB (via relay): !bbs qsl post
→ Posted QSL #21 - 1 hop away, SNR: 8

NodeC (via relay): !bbs qsl post
→ Posted QSL #22 - 2 hops away, SNR: 4
```

Shows how signal degrades through the relay chain.

## Technical Details

### What Information Is Captured

When you post a QSL, these fields are automatically captured:

| Field | Source | Notes |
|-------|--------|-------|
| QSL ID | Auto-incremented | Sequential number |
| Sender | Incoming packet | Who posted the QSL |
| Sender Name | Node database | Short name of sender |
| Hop Count | Packet header | 0=direct, 1=1 relay, etc. |
| SNR | Packet header | Signal-to-Noise Ratio (0-15) |
| RSSI | Packet header | Received Signal Strength (-120 to -40 dBm) |
| GPS Location | Node position | Optional, if GPS available |
| Timestamp | System time | When post was created |

### Data Retention

- **PSRAM boards** (Heltec V3): 200 posts, lost on reboot (fast memory)
- **Flash boards** (RAK4631, etc): 50 posts, persist across reboots
- **Auto-cleanup**: Posts older than 24 hours are automatically removed

### Bandwidth Usage

Each QSL post is minimal:
- Post creation: Small text command (~5 bytes)
- Transmission: Only metadata (no large message content)
- Board listing: Headers only (~15 bytes per entry)

## Tips & Tricks

### Finding Distant Nodes

Post QSL and view the board to find:
- Nodes with high hop counts (distant)
- Nodes with poor SNR (weak signal)
- Nodes that only connect through specific relays

### Identifying Link Issues

Compare QSL posts between your node and others:
- Same hop count but different SNR → antenna issue
- High hop count → missing intermediate relay
- No QSL received → possible RF blockage

### Coverage Mapping

Combine QSL location data with hop counts to create a mental map:
- QSL posts with "has location" can be plotted
- Hop count shows path distance
- SNR shows signal quality along that path

### Temporary Networks

In deployments or field events, QSL board shows:
- Current active nodes
- Network topology
- Real-time connectivity status

---

**Remember**: QSL posts are PUBLIC. Don't post sensitive location data if privacy is a concern. Consider using generic names or approximate locations on public networks.
