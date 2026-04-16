#!/usr/bin/env python3
"""
gen_rpg_packed.py — Generate packed binary content files for Fallout Wasteland RPG.

Reads JSON data from rpg_data/ and produces compact binary files for nRF52 external flash.

Output files (in data/):
  enemies.bin   — enemy/creature data with string pool
  items.bin     — weapon/armor/consumable/junk/ammo data
  locations.bin — named locations with enemy refs
  flavor.bin    — room descriptions, find text, ambient, NPC lines

Binary format uses a shared string pool per file to minimize duplication.
All multi-byte integers are little-endian.

Target: ~500KB total across all four files.
"""

import json
import os
import struct
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
DATA_DIR = SCRIPT_DIR / "rpg_data"
OUT_DIR = SCRIPT_DIR / "data"

# Magic numbers
MAGIC_ENEMIES   = 0x454E454D  # "ENEM"
MAGIC_ITEMS     = 0x4954454D  # "ITEM"
MAGIC_LOCATIONS = 0x4C4F4341  # "LOCA"
MAGIC_FLAVOR    = 0x464C4156  # "FLAV"

# Pool limits — string offsets in entries are uint16 (max 65535).
# Keep pool per section under this threshold.
MAX_POOL_BYTES = 63000
# Soft caps: best entries win, by rarity/danger sort.
MAX_ITEMS      = 700
MAX_LOCATIONS  = 700
DESC_CAP       = 60  # truncate descriptions at this many chars


def cap_str(s: str, limit: int = DESC_CAP) -> str:
    """Truncate s to at most `limit` chars at a word boundary."""
    if not s or len(s) <= limit:
        return s
    cut = s[:limit].rsplit(' ', 1)[0].rstrip(',;:')
    return cut + '...'

# ─── String Pool ──────────────────────────────────────────────────────────────

class StringPool:
    """Deduplicated string pool. Returns offset for each unique string."""
    def __init__(self):
        self.strings = {}  # str -> offset
        self.data = bytearray()

    def add(self, s: str) -> int:
        if not s:
            return 0xFFFF  # sentinel for empty/null
        if s in self.strings:
            return self.strings[s]
        encoded = s.encode('utf-8')
        if len(encoded) > 255:
            encoded = encoded[:255]
        offset = len(self.data)
        self.data.append(len(encoded))
        self.data.extend(encoded)
        self.strings[s] = offset
        return offset

    def size(self):
        return len(self.data)

    def bytes(self):
        return bytes(self.data)


# ─── Faction / Type Encoding ─────────────────────────────────────────────────

FACTION_MAP = {
    "creature": 0, "ghoul": 1, "raider": 2, "mutant": 3,
    "robot": 4, "human": 5
}

ITEM_TYPE_MAP = {
    "weapon": 0, "armor": 1, "consumable": 2, "junk": 3, "ammo": 4
}

LOCATION_TYPE_MAP = {
    "vault": 0, "wasteland": 1, "raider_camp": 2, "ghoul_ruin": 3,
    "military": 4, "town": 5, "cave": 6
}

TONE_MAP = {
    "grim": 0, "hopeful": 1, "humorous": 2, "tense": 3
}

GAME_MAP = {
    "FO1": 0x01, "FO2": 0x02, "FO3": 0x04, "NV": 0x08, "FO4": 0x10
}

def encode_game_sources(sources: list) -> int:
    """Encode game sources as a bitmask."""
    mask = 0
    for s in sources:
        mask |= GAME_MAP.get(s, 0)
    return mask


# ─── Enemies ──────────────────────────────────────────────────────────────────

def build_enemies():
    """
    enemies.bin format:
      Header:
        magic         uint32
        count         uint16
        pool_offset   uint32  (offset to string pool from start of file)
      Entry[count]:  (24 bytes each)
        name_off      uint16  (offset into string pool)
        desc_off      uint16
        game_mask     uint8
        faction       uint8
        difficulty    uint8
        defense_rating uint8
        hp_min        uint16
        hp_max        uint16
        damage_min    uint16
        damage_max    uint16
        xp_reward     uint16
        loot_count    uint8
        loot[3]:      (each: name_off uint16 + weight uint8 = 3 bytes)
        _pad          uint8   (align to 24+9=33... let's use fixed 36 bytes)
      StringPool
    """
    with open(DATA_DIR / "enemies.json") as f:
        enemies = json.load(f)

    pool = StringPool()
    entries = bytearray()

    for e in enemies:
        name_off = pool.add(e["name"])
        desc_off = pool.add(e.get("description", ""))
        game_mask = encode_game_sources(e.get("game_source", []))
        faction = FACTION_MAP.get(e.get("faction", "creature"), 0)
        difficulty = e.get("difficulty", 1)
        defense = min(e.get("defense_rating", 0), 255)
        hp_min = min(e.get("hp_min", 10), 65535)
        hp_max = min(e.get("hp_max", 20), 65535)
        dmg_min = min(e.get("damage_min", 1), 65535)
        dmg_max = min(e.get("damage_max", 5), 65535)
        xp = min(e.get("xp_reward", 10), 65535)

        loot = e.get("loot_table", [])[:3]  # max 3 loot entries
        loot_count = len(loot)

        # Pack entry: 20 bytes fixed + 9 bytes loot (3x3) + 1 pad = 30 bytes
        entry = struct.pack('<HH BBB B HH HH H B',
            name_off, desc_off,
            game_mask, faction, difficulty, defense,
            hp_min, hp_max,
            dmg_min, dmg_max,
            xp, loot_count
        )
        # 3 loot slots (name_off uint16 + weight uint8 = 3 bytes each)
        for i in range(3):
            if i < len(loot):
                lname_off = pool.add(loot[i].get("item", ""))
                lweight = min(loot[i].get("weight", 50), 255)
                entry += struct.pack('<HB', lname_off, lweight)
            else:
                entry += struct.pack('<HB', 0xFFFF, 0)
        # pad to 30 bytes total (19 fixed + 9 loot + 2 pad)
        entry += b'\x00\x00'
        assert len(entry) == 30, f"Enemy entry size {len(entry)} != 30"
        entries.extend(entry)

    # Build file
    header_size = 4 + 2 + 4  # magic + count + pool_offset
    pool_offset = header_size + len(entries)

    out = struct.pack('<IHI', MAGIC_ENEMIES, len(enemies), pool_offset)
    out += bytes(entries)
    out += pool.bytes()
    return out, len(enemies), pool.size()


# ─── Items ────────────────────────────────────────────────────────────────────

def build_items():
    """
    items.bin format:
      Header:
        magic         uint32
        count         uint16
        pool_offset   uint32
      Entry[count]:  (16 bytes each)
        name_off      uint16
        desc_off      uint16
        effect_off    uint16  (damage/effect string)
        game_mask     uint8
        item_type     uint8
        rarity        uint8
        _pad          uint8
        weight_x10    uint16  (weight * 10, integer)
        value         uint16
      StringPool
    """
    with open(DATA_DIR / "items.json") as f:
        items = json.load(f)

    # Best items first: rarity desc, then value desc
    items.sort(key=lambda x: (-x.get('rarity', 1), -x.get('value', 0)))
    items = items[:MAX_ITEMS]

    pool = StringPool()
    entries = bytearray()

    for item in items:
        name_off = pool.add(item["name"])
        desc_off = pool.add(cap_str(item.get("description", "")))
        effect_off = pool.add(item.get("damage", ""))
        game_mask = encode_game_sources(item.get("game_source", []))
        item_type = ITEM_TYPE_MAP.get(item.get("type", "junk"), 3)
        rarity = item.get("rarity", 1)
        weight_x10 = int(item.get("weight", 0) * 10)
        value = min(item.get("value", 0), 65535)

        entry = struct.pack('<HHH BB BB HH H',
            name_off, desc_off, effect_off,
            game_mask, item_type,
            rarity, 0,
            weight_x10, value,
            0  # pad
        )
        assert len(entry) == 16, f"Item entry size {len(entry)} != 16"
        entries.extend(entry)

    header_size = 4 + 2 + 4
    pool_offset = header_size + len(entries)

    out = struct.pack('<IHI', MAGIC_ITEMS, len(items), pool_offset)
    out += bytes(entries)
    out += pool.bytes()
    return out, len(items), pool.size()


# ─── Locations ────────────────────────────────────────────────────────────────

def build_locations():
    """
    locations.bin format:
      Header:
        magic         uint32
        count         uint16
        pool_offset   uint32
      Entry[count]:  (20 bytes each)
        name_off      uint16
        desc_off      uint16
        game_mask     uint8
        loc_type      uint8
        danger_level  uint8
        enemy_count   uint8
        enemies[4]:   uint16 each (name_off in pool, max 4)
        _pad          uint16 (align to 20)
      StringPool
    """
    with open(DATA_DIR / "locations.json") as f:
        locations = json.load(f)

    # Best locations first: non-wasteland types first, then danger desc, then name
    LOC_TYPE_PRIORITY = {"vault": 0, "military": 1, "raider_camp": 2,
                         "ghoul_ruin": 3, "cave": 4, "town": 5, "wasteland": 6}
    locations.sort(key=lambda x: (
        LOC_TYPE_PRIORITY.get(x.get('type', 'wasteland'), 6),
        -x.get('danger_level', 1),
        x.get('name', '')
    ))
    locations = locations[:MAX_LOCATIONS]

    pool = StringPool()
    entries = bytearray()

    for loc in locations:
        name_off = pool.add(loc["name"])
        desc_off = pool.add(cap_str(loc.get("description", "")))
        game_mask = encode_game_sources(loc.get("game_source", []))
        loc_type = LOCATION_TYPE_MAP.get(loc.get("type", "wasteland"), 1)
        danger = loc.get("danger_level", 1)
        enemies = loc.get("typical_enemies", [])[:4]
        enemy_count = len(enemies)

        entry = struct.pack('<HH BBB B',
            name_off, desc_off,
            game_mask, loc_type, danger, enemy_count
        )
        for i in range(4):
            if i < len(enemies):
                entry += struct.pack('<H', pool.add(enemies[i]))
            else:
                entry += struct.pack('<H', 0xFFFF)
        entry += struct.pack('<H', 0)  # pad
        assert len(entry) == 18, f"Location entry size {len(entry)} != 18"
        entries.extend(entry)

    header_size = 4 + 2 + 4
    pool_offset = header_size + len(entries)

    out = struct.pack('<IHI', MAGIC_LOCATIONS, len(locations), pool_offset)
    out += bytes(entries)
    out += pool.bytes()
    return out, len(locations), pool.size()


# ─── Flavor ───────────────────────────────────────────────────────────────────

def build_flavor():
    """
    flavor.bin format:
      Header:
        magic             uint32
        room_count        uint16
        find_count        uint16
        ambient_count     uint16
        npc_count         uint16
        pool_offset       uint32
      RoomEntry[room_count]:  (6 bytes each)
        text_off          uint16
        loc_type          uint8
        tone              uint8
        game_mask         uint8
        _pad              uint8
      FindEntry[find_count]:  (4 bytes each)
        text_off          uint16
        tone              uint8
        _pad              uint8
      AmbientEntry[ambient_count]:  (4 bytes each)
        text_off          uint16
        tone              uint8
        _pad              uint8
      NPCEntry[npc_count]:  (6 bytes each)
        text_off          uint16
        tone              uint8
        game_mask         uint8
        _pad              uint16
      StringPool
    """
    with open(DATA_DIR / "flavor.json") as f:
        flavor = json.load(f)

    pool = StringPool()
    entries = bytearray()

    # Cap per-section counts so total pool stays under uint16 limit.
    # Total entries capped at ~900; text capped at DESC_CAP chars.
    rooms    = flavor.get("room_descriptions", [])[:100]
    finds    = flavor.get("find_descriptions", [])[:250]
    ambients = flavor.get("ambient_descriptions", [])[:500]
    npcs     = flavor.get("npc_lines", [])[:100]

    # Room entries (6 bytes each)
    for r in rooms:
        text_off = pool.add(cap_str(r["text"], 100))
        loc_type = LOCATION_TYPE_MAP.get(r.get("location_type", "wasteland"), 1)
        tone = TONE_MAP.get(r.get("tone", "grim"), 0)
        game_mask = GAME_MAP.get(r.get("source_game", ""), 0)
        entries.extend(struct.pack('<H BBB x', text_off, loc_type, tone, game_mask))

    # Find entries (4 bytes each)
    for f_entry in finds:
        text_off = pool.add(cap_str(f_entry["text"], 100))
        tone = TONE_MAP.get(f_entry.get("tone", "grim"), 0)
        entries.extend(struct.pack('<H B x', text_off, tone))

    # Ambient entries (4 bytes each)
    for a in ambients:
        text_off = pool.add(cap_str(a["text"], 100))
        tone = TONE_MAP.get(a.get("tone", "grim"), 0)
        entries.extend(struct.pack('<H B x', text_off, tone))

    # NPC entries (6 bytes each)
    for n in npcs:
        text_off = pool.add(cap_str(n["text"], 100))
        tone = TONE_MAP.get(n.get("tone", "grim"), 0)
        game_mask = GAME_MAP.get(n.get("source_game", ""), 0)
        entries.extend(struct.pack('<H BB xx', text_off, tone, game_mask))

    header_size = 4 + 2*4 + 4  # magic + 4x uint16 + pool_offset
    pool_offset = header_size + len(entries)

    out = struct.pack('<I HHHH I',
        MAGIC_FLAVOR,
        len(rooms), len(finds), len(ambients), len(npcs),
        pool_offset
    )
    out += bytes(entries)
    out += pool.bytes()
    return out, len(rooms), len(finds), len(ambients), len(npcs), pool.size()


# ─── C Header Generation ─────────────────────────────────────────────────────

def generate_c_header():
    return '''#pragma once
// content_tables.h — Struct definitions for RPG binary content files.
// Generated by gen_rpg_packed.py. Do NOT edit manually.
//
// All files use little-endian byte order.
// String pool entries: uint8 length + char[length] (no null terminator in pool).
// 0xFFFF = null/empty string offset.

#include <cstdint>

// ─── Game source bitmask ──────────────────────────────────────────────────
#define GAME_FO1  0x01
#define GAME_FO2  0x02
#define GAME_FO3  0x04
#define GAME_NV   0x08
#define GAME_FO4  0x10

// ─── Faction codes ────────────────────────────────────────────────────────
#define FACTION_CREATURE  0
#define FACTION_GHOUL     1
#define FACTION_RAIDER    2
#define FACTION_MUTANT    3
#define FACTION_ROBOT     4
#define FACTION_HUMAN     5

// ─── Item types ───────────────────────────────────────────────────────────
#define ITYPE_WEAPON      0
#define ITYPE_ARMOR       1
#define ITYPE_CONSUMABLE  2
#define ITYPE_JUNK        3
#define ITYPE_AMMO        4

// ─── Location types ───────────────────────────────────────────────────────
#define LTYPE_VAULT       0
#define LTYPE_WASTELAND   1
#define LTYPE_RAIDER_CAMP 2
#define LTYPE_GHOUL_RUIN  3
#define LTYPE_MILITARY    4
#define LTYPE_TOWN        5
#define LTYPE_CAVE        6

// ─── Tone codes ───────────────────────────────────────────────────────────
#define TONE_GRIM     0
#define TONE_HOPEFUL  1
#define TONE_HUMOROUS 2
#define TONE_TENSE    3

// ─── File magic numbers ──────────────────────────────────────────────────
#define MAGIC_ENEMIES   0x454E454D
#define MAGIC_ITEMS     0x4954454D
#define MAGIC_LOCATIONS 0x4C4F4341
#define MAGIC_FLAVOR    0x464C4156

// ─── Enemy loot entry (3 bytes) ──────────────────────────────────────────
struct __attribute__((packed)) RPGLootEntry {
    uint16_t name_off;   // string pool offset (0xFFFF = empty)
    uint8_t  weight;     // drop weight 0-255
};

// ─── Enemy entry (30 bytes) ──────────────────────────────────────────────
struct __attribute__((packed)) RPGEnemyEntry {
    uint16_t name_off;
    uint16_t desc_off;
    uint8_t  game_mask;
    uint8_t  faction;
    uint8_t  difficulty;    // 1-4
    uint8_t  defense_rating;
    uint16_t hp_min;
    uint16_t hp_max;
    uint16_t damage_min;
    uint16_t damage_max;
    uint16_t xp_reward;
    uint8_t  loot_count;
    RPGLootEntry loot[3];
    uint8_t  _pad;
};

// ─── Item entry (16 bytes) ───────────────────────────────────────────────
struct __attribute__((packed)) RPGItemEntry {
    uint16_t name_off;
    uint16_t desc_off;
    uint16_t effect_off;   // damage/effect string
    uint8_t  game_mask;
    uint8_t  item_type;    // ITYPE_*
    uint8_t  rarity;       // 1-3
    uint8_t  _pad;
    uint16_t weight_x10;   // weight * 10
    uint16_t value;        // caps
};

// ─── Location entry (18 bytes) ───────────────────────────────────────────
struct __attribute__((packed)) RPGLocationEntry {
    uint16_t name_off;
    uint16_t desc_off;
    uint8_t  game_mask;
    uint8_t  loc_type;     // LTYPE_*
    uint8_t  danger_level; // 1-4
    uint8_t  enemy_count;
    uint16_t enemies[4];   // string pool offsets of enemy names
    uint16_t _pad;
};

// ─── Flavor: room description (6 bytes) ──────────────────────────────────
struct __attribute__((packed)) RPGRoomDesc {
    uint16_t text_off;
    uint8_t  loc_type;
    uint8_t  tone;
    uint8_t  game_mask;
    uint8_t  _pad;
};

// ─── Flavor: find/ambient description (4 bytes) ─────────────────────────
struct __attribute__((packed)) RPGFindDesc {
    uint16_t text_off;
    uint8_t  tone;
    uint8_t  _pad;
};

// ─── Flavor: NPC line (6 bytes) ──────────────────────────────────────────
struct __attribute__((packed)) RPGNPCLine {
    uint16_t text_off;
    uint8_t  tone;
    uint8_t  game_mask;
    uint16_t _pad;
};

// ─── File headers ────────────────────────────────────────────────────────

struct __attribute__((packed)) RPGEnemyHeader {
    uint32_t magic;        // MAGIC_ENEMIES
    uint16_t count;
    uint32_t pool_offset;  // byte offset from file start to string pool
    // followed by RPGEnemyEntry[count], then string pool
};

struct __attribute__((packed)) RPGItemHeader {
    uint32_t magic;        // MAGIC_ITEMS
    uint16_t count;
    uint32_t pool_offset;
};

struct __attribute__((packed)) RPGLocationHeader {
    uint32_t magic;        // MAGIC_LOCATIONS
    uint16_t count;
    uint32_t pool_offset;
};

struct __attribute__((packed)) RPGFlavorHeader {
    uint32_t magic;        // MAGIC_FLAVOR
    uint16_t room_count;
    uint16_t find_count;
    uint16_t ambient_count;
    uint16_t npc_count;
    uint32_t pool_offset;
    // followed by RPGRoomDesc[room_count], RPGFindDesc[find_count],
    //            RPGFindDesc[ambient_count], RPGNPCLine[npc_count], then string pool
};

// ─── String pool reader helper ───────────────────────────────────────────
// Read a string from the pool at the given offset.
// Pool format: uint8 length + char[length] (NOT null-terminated in pool).
// Returns pointer and sets outLen. Caller must bounds-check.
inline const char* rpgPoolString(const uint8_t *pool, uint16_t offset, uint8_t &outLen) {
    if (offset == 0xFFFF) { outLen = 0; return nullptr; }
    outLen = pool[offset];
    return (const char *)&pool[offset + 1];
}
'''


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    print("=== Fallout RPG Content Pack Generator ===\n")

    # Build all files
    enemies_bin, e_count, e_pool = build_enemies()
    items_bin, i_count, i_pool = build_items()
    locations_bin, l_count, l_pool = build_locations()
    flavor_result = build_flavor()
    flavor_bin = flavor_result[0]
    f_rooms, f_finds, f_ambients, f_npcs, f_pool = flavor_result[1:]

    # Write binary files
    for name, data in [
        ("enemies.bin", enemies_bin),
        ("items.bin", items_bin),
        ("locations.bin", locations_bin),
        ("flavor.bin", flavor_bin),
    ]:
        path = OUT_DIR / name
        with open(path, 'wb') as f:
            f.write(data)
        print(f"  {name:20s} {len(data):>7,} bytes")

    total = len(enemies_bin) + len(items_bin) + len(locations_bin) + len(flavor_bin)
    print(f"\n  {'TOTAL':20s} {total:>7,} bytes")
    budget = 500 * 1024
    print(f"  {'Budget':20s} {budget:>7,} bytes")
    pct = total / budget * 100
    print(f"  {'Usage':20s} {pct:>6.1f}%")

    if total > budget:
        print("\n  ⚠ WARNING: Over budget!")
    else:
        print(f"\n  ✓ Under budget by {budget - total:,} bytes")

    # Write C header
    header_path = SCRIPT_DIR / ".." / "module-src" / "content_tables.h"
    with open(header_path, 'w') as f:
        f.write(generate_c_header())
    print(f"\n  C header: {header_path}")

    # Summary
    print(f"\n=== Content Summary ===")
    print(f"  Enemies:     {e_count:>4}  (pool: {e_pool:,} bytes)")
    print(f"  Items:       {i_count:>4}  (pool: {i_pool:,} bytes)")
    print(f"  Locations:   {l_count:>4}  (pool: {l_pool:,} bytes)")
    print(f"  Flavor text:")
    print(f"    Rooms:     {f_rooms:>4}")
    print(f"    Finds:     {f_finds:>4}")
    print(f"    Ambient:   {f_ambients:>4}")
    print(f"    NPC lines: {f_npcs:>4}")
    print(f"    (pool: {f_pool:,} bytes)")

    # Game coverage
    print(f"\n=== Game Coverage ===")
    with open(DATA_DIR / "enemies.json") as f:
        enemies = json.load(f)
    with open(DATA_DIR / "items.json") as f:
        items = json.load(f)
    with open(DATA_DIR / "locations.json") as f:
        locations = json.load(f)

    for game in ["FO1", "FO2", "FO3", "NV", "FO4"]:
        ec = sum(1 for e in enemies if game in e.get("game_source", []))
        ic = sum(1 for i in items if game in i.get("game_source", []))
        lc = sum(1 for l in locations if game in l.get("game_source", []))
        print(f"  {game:4s}: {ec:>3} enemies, {ic:>3} items, {lc:>3} locations")


if __name__ == "__main__":
    main()
