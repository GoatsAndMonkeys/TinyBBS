#pragma once
// FRPGContent.h — Flash-loaded content for Fallout Wasteland RPG.
// Reads binary data files from external flash (nRF52) or LittleFS (ESP32).
// Falls back gracefully if files are missing.

#include "content_tables.h"
#include "FRPGRegions.h"
#ifdef NRF52_SERIES
#include "BBSExtFlash.h"
#else
// ESP32: use FSCom instead of external flash
#include "FSCommon.h"
static inline fs::FS &bbsExtFS() { return FSCom; }
#endif
#include "FSCommon.h"
#include <cstring>

#define FRPG_BIN_DIR       "/bbs/kb"
#define FRPG_ENEMIES_PATH  "/bbs/kb/enemies.bin"
#define FRPG_ITEMS_PATH    "/bbs/kb/items.bin"
#define FRPG_LOCS_PATH     "/bbs/kb/locations.bin"
#define FRPG_FLAVOR_PATH   "/bbs/kb/flavor.bin"

#define FRPG_CONTENT_ENEMIES   0x01
#define FRPG_CONTENT_ITEMS     0x02
#define FRPG_CONTENT_LOCATIONS 0x04
#define FRPG_CONTENT_FLAVOR    0x08

// ─── Cached index (populated at init) ────────────────────────────────────────

struct FRPGContentIndex {
    // enemies.bin
    uint16_t enemyCount;
    uint32_t enemyPoolOff;
    uint16_t enemiesPerTier[4]; // count per difficulty 1-4
    // items.bin
    uint16_t itemCount;
    uint32_t itemPoolOff;
    // locations.bin
    uint16_t locCount;
    uint32_t locPoolOff;
    uint16_t locsPerDanger[4]; // count per danger_level 1-4
    // flavor.bin
    uint16_t roomCount, findCount, ambientCount, npcCount;
    uint32_t flavorPoolOff;
    uint32_t flavorRoomOff, flavorFindOff, flavorAmbientOff, flavorNpcOff;
    // availability
    uint8_t  available; // bitmask FRPG_CONTENT_*
    bool     initialized;
};

static FRPGContentIndex _rpgIdx = {};

// ─── String pool reader ──────────────────────────────────────────────────────

static char _rpgStrBuf[80];

static const char* rpgReadStr(File &f, uint32_t poolBase, uint16_t strOff) {
    if (strOff == 0xFFFF) return "";
    f.seek(poolBase + strOff);
    uint8_t slen = 0;
    if (f.read(&slen, 1) != 1) return "";
    if (slen >= sizeof(_rpgStrBuf)) slen = sizeof(_rpgStrBuf) - 1;
    f.read((uint8_t *)_rpgStrBuf, slen);
    _rpgStrBuf[slen] = '\0';
    return _rpgStrBuf;
}

// Read a pool string into a caller-provided buffer (for when you need multiple strings)
static void rpgReadStrTo(File &f, uint32_t poolBase, uint16_t strOff, char *dst, size_t dstLen) {
    if (strOff == 0xFFFF || dstLen == 0) { if (dstLen) dst[0] = '\0'; return; }
    f.seek(poolBase + strOff);
    uint8_t slen = 0;
    if (f.read(&slen, 1) != 1) { dst[0] = '\0'; return; }
    if (slen >= dstLen) slen = (uint8_t)(dstLen - 1);
    f.read((uint8_t *)dst, slen);
    dst[slen] = '\0';
}

// ─── Init ────────────────────────────────────────────────────────────────────

static void frpgContentInit() {
    if (_rpgIdx.initialized) return;
    _rpgIdx.initialized = true;
    _rpgIdx.available = 0;
    memset(_rpgIdx.enemiesPerTier, 0, sizeof(_rpgIdx.enemiesPerTier));
    memset(_rpgIdx.locsPerDanger, 0, sizeof(_rpgIdx.locsPerDanger));

    // ── enemies.bin ──
    {
        File f = bbsExtFS().open(FRPG_ENEMIES_PATH, FILE_O_READ);
        if (f) {
            RPGEnemyHeader hdr;
            if (f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) &&
                hdr.magic == MAGIC_ENEMIES && hdr.count > 0) {
                _rpgIdx.enemyCount = hdr.count;
                _rpgIdx.enemyPoolOff = hdr.pool_offset;
                _rpgIdx.available |= FRPG_CONTENT_ENEMIES;
                // Build per-tier counts by scanning difficulty byte
                for (uint16_t i = 0; i < hdr.count; i++) {
                    uint32_t entryOff = sizeof(RPGEnemyHeader) + (uint32_t)i * 30;
                    f.seek(entryOff + 6); // difficulty is at offset 6 within entry
                    uint8_t diff = 0;
                    f.read(&diff, 1);
                    if (diff >= 1 && diff <= 4) _rpgIdx.enemiesPerTier[diff - 1]++;
                }
            }
            f.close();
        }
    }

    // ── items.bin ──
    {
        File f = bbsExtFS().open(FRPG_ITEMS_PATH, FILE_O_READ);
        if (f) {
            RPGItemHeader hdr;
            if (f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) &&
                hdr.magic == MAGIC_ITEMS && hdr.count > 0) {
                _rpgIdx.itemCount = hdr.count;
                _rpgIdx.itemPoolOff = hdr.pool_offset;
                _rpgIdx.available |= FRPG_CONTENT_ITEMS;
            }
            f.close();
        }
    }

    // ── locations.bin ──
    {
        File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
        if (f) {
            RPGLocationHeader hdr;
            if (f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) &&
                hdr.magic == MAGIC_LOCATIONS && hdr.count > 0) {
                _rpgIdx.locCount = hdr.count;
                _rpgIdx.locPoolOff = hdr.pool_offset;
                _rpgIdx.available |= FRPG_CONTENT_LOCATIONS;
                // Build per-danger counts
                for (uint16_t i = 0; i < hdr.count; i++) {
                    uint32_t entryOff = sizeof(RPGLocationHeader) + (uint32_t)i * 18;
                    f.seek(entryOff + 6); // danger_level at offset 6
                    uint8_t dl = 0;
                    f.read(&dl, 1);
                    if (dl >= 1 && dl <= 4) _rpgIdx.locsPerDanger[dl - 1]++;
                }
            }
            f.close();
        }
    }

    // ── flavor.bin ──
    {
        File f = bbsExtFS().open(FRPG_FLAVOR_PATH, FILE_O_READ);
        if (f) {
            RPGFlavorHeader hdr;
            if (f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) &&
                hdr.magic == MAGIC_FLAVOR) {
                _rpgIdx.roomCount = hdr.room_count;
                _rpgIdx.findCount = hdr.find_count;
                _rpgIdx.ambientCount = hdr.ambient_count;
                _rpgIdx.npcCount = hdr.npc_count;
                _rpgIdx.flavorPoolOff = hdr.pool_offset;
                // Compute section offsets
                uint32_t off = sizeof(RPGFlavorHeader);
                _rpgIdx.flavorRoomOff = off;    off += (uint32_t)hdr.room_count * 6;
                _rpgIdx.flavorFindOff = off;    off += (uint32_t)hdr.find_count * 4;
                _rpgIdx.flavorAmbientOff = off; off += (uint32_t)hdr.ambient_count * 4;
                _rpgIdx.flavorNpcOff = off;
                _rpgIdx.available |= FRPG_CONTENT_FLAVOR;
            }
            f.close();
        }
    }
}

static bool frpgContentHas(uint8_t bit) { return (_rpgIdx.available & bit) != 0; }

// ─── Enemy access ────────────────────────────────────────────────────────────

// Read a full enemy entry by index. Returns false on failure.
static bool frpgReadEnemy(uint16_t idx, RPGEnemyEntry &out) {
    if (idx >= _rpgIdx.enemyCount) return false;
    File f = bbsExtFS().open(FRPG_ENEMIES_PATH, FILE_O_READ);
    if (!f) return false;
    uint32_t off = sizeof(RPGEnemyHeader) + (uint32_t)idx * 30;
    f.seek(off);
    bool ok = (f.read((uint8_t *)&out, 30) == 30);
    f.close();
    return ok;
}

// Read just the enemy name into dst.
static void frpgReadEnemyName(uint16_t idx, char *dst, size_t dstLen) {
    dst[0] = '\0';
    RPGEnemyEntry e;
    if (!frpgReadEnemy(idx, e)) return;
    File f = bbsExtFS().open(FRPG_ENEMIES_PATH, FILE_O_READ);
    if (!f) return;
    rpgReadStrTo(f, _rpgIdx.enemyPoolOff, e.name_off, dst, dstLen);
    f.close();
}

// Pick a random enemy index for the given tier (difficulty 1-4).
// Returns 0 on failure (caller should check content availability).
static uint16_t frpgPickTierEnemy(uint8_t tier) {
    if (tier < 1 || tier > 4) tier = 1;
    uint16_t count = _rpgIdx.enemiesPerTier[tier - 1];
    if (count == 0) return 0;
    uint16_t pick = (uint16_t)random(count);
    // Scan entries counting matches
    File f = bbsExtFS().open(FRPG_ENEMIES_PATH, FILE_O_READ);
    if (!f) return 0;
    uint16_t found = 0;
    uint16_t result = 0;
    for (uint16_t i = 0; i < _rpgIdx.enemyCount; i++) {
        uint32_t off = sizeof(RPGEnemyHeader) + (uint32_t)i * 30 + 6; // difficulty byte
        f.seek(off);
        uint8_t diff = 0;
        f.read(&diff, 1);
        if (diff == tier) {
            if (found == pick) { result = i; break; }
            found++;
        }
    }
    f.close();
    return result;
}

// Map faction to ETYPE for VATS targeting
static uint8_t frpgFactionToEtype(uint8_t faction) {
    if (faction == FACTION_CREATURE) return 1; // ETYPE_CREATURE
    if (faction == FACTION_ROBOT)    return 2; // ETYPE_ROBOT
    return 0; // ETYPE_HUMANOID
}

// ─── Location access ─────────────────────────────────────────────────────────

static bool frpgReadLocation(uint16_t idx, RPGLocationEntry &out) {
    if (idx >= _rpgIdx.locCount) return false;
    File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
    if (!f) return false;
    uint32_t off = sizeof(RPGLocationHeader) + (uint32_t)idx * 18;
    f.seek(off);
    bool ok = (f.read((uint8_t *)&out, 18) == 18);
    f.close();
    return ok;
}

static uint16_t frpgPickLocation(uint8_t danger) {
    if (danger < 1 || danger > 4) danger = 1;
    uint16_t count = _rpgIdx.locsPerDanger[danger - 1];
    if (count == 0) {
        // Fall back: try any adjacent danger level
        for (int d = 1; d <= 4; d++) {
            if (_rpgIdx.locsPerDanger[d - 1] > 0) { danger = d; count = _rpgIdx.locsPerDanger[d - 1]; break; }
        }
        if (count == 0) return 0;
    }
    uint16_t pick = (uint16_t)random(count);
    File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
    if (!f) return 0;
    uint16_t found = 0;
    uint16_t result = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        uint32_t off = sizeof(RPGLocationHeader) + (uint32_t)i * 18 + 6; // danger_level
        f.seek(off);
        uint8_t dl = 0;
        f.read(&dl, 1);
        if (dl == danger) {
            if (found == pick) { result = i; break; }
            found++;
        }
    }
    f.close();
    return result;
}

static void frpgReadLocationName(uint16_t idx, char *dst, size_t dstLen) {
    dst[0] = '\0';
    RPGLocationEntry loc;
    if (!frpgReadLocation(idx, loc)) return;
    File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
    if (!f) return;
    rpgReadStrTo(f, _rpgIdx.locPoolOff, loc.name_off, dst, dstLen);
    f.close();
}

// ─── Flavor text access ──────────────────────────────────────────────────────

// Read a random room description matching location type. Returns false if none.
static bool frpgReadRoomDesc(uint8_t locType, char *dst, size_t dstLen) {
    dst[0] = '\0';
    if (!frpgContentHas(FRPG_CONTENT_FLAVOR) || _rpgIdx.roomCount == 0) return false;

    // Count matching rooms (by loc_type at offset 2 within 6-byte entry)
    File f = bbsExtFS().open(FRPG_FLAVOR_PATH, FILE_O_READ);
    if (!f) return false;

    uint16_t matchCount = 0;
    for (uint16_t i = 0; i < _rpgIdx.roomCount; i++) {
        f.seek(_rpgIdx.flavorRoomOff + (uint32_t)i * 6 + 2);
        uint8_t lt = 0;
        f.read(&lt, 1);
        if (lt == locType) matchCount++;
    }
    if (matchCount == 0) {
        // Fall back to any room
        matchCount = _rpgIdx.roomCount;
        uint16_t pick = (uint16_t)random(matchCount);
        f.seek(_rpgIdx.flavorRoomOff + (uint32_t)pick * 6);
        RPGRoomDesc rd;
        f.read((uint8_t *)&rd, 6);
        rpgReadStrTo(f, _rpgIdx.flavorPoolOff, rd.text_off, dst, dstLen);
        f.close();
        return true;
    }
    uint16_t pick = (uint16_t)random(matchCount);
    uint16_t found = 0;
    for (uint16_t i = 0; i < _rpgIdx.roomCount; i++) {
        f.seek(_rpgIdx.flavorRoomOff + (uint32_t)i * 6 + 2);
        uint8_t lt = 0;
        f.read(&lt, 1);
        if (lt == locType) {
            if (found == pick) {
                f.seek(_rpgIdx.flavorRoomOff + (uint32_t)i * 6);
                RPGRoomDesc rd;
                f.read((uint8_t *)&rd, 6);
                rpgReadStrTo(f, _rpgIdx.flavorPoolOff, rd.text_off, dst, dstLen);
                f.close();
                return true;
            }
            found++;
        }
    }
    f.close();
    return false;
}

// Read a random find description
static bool frpgReadFindDesc(char *dst, size_t dstLen) {
    dst[0] = '\0';
    if (!frpgContentHas(FRPG_CONTENT_FLAVOR) || _rpgIdx.findCount == 0) return false;
    uint16_t pick = (uint16_t)random(_rpgIdx.findCount);
    File f = bbsExtFS().open(FRPG_FLAVOR_PATH, FILE_O_READ);
    if (!f) return false;
    f.seek(_rpgIdx.flavorFindOff + (uint32_t)pick * 4);
    RPGFindDesc fd;
    f.read((uint8_t *)&fd, 4);
    rpgReadStrTo(f, _rpgIdx.flavorPoolOff, fd.text_off, dst, dstLen);
    f.close();
    return true;
}

// Read a random NPC line
static bool frpgReadNPCLine(char *dst, size_t dstLen) {
    dst[0] = '\0';
    if (!frpgContentHas(FRPG_CONTENT_FLAVOR) || _rpgIdx.npcCount == 0) return false;
    uint16_t pick = (uint16_t)random(_rpgIdx.npcCount);
    File f = bbsExtFS().open(FRPG_FLAVOR_PATH, FILE_O_READ);
    if (!f) return false;
    f.seek(_rpgIdx.flavorNpcOff + (uint32_t)pick * 6);
    RPGNPCLine nl;
    f.read((uint8_t *)&nl, 6);
    rpgReadStrTo(f, _rpgIdx.flavorPoolOff, nl.text_off, dst, dstLen);
    f.close();
    return true;
}

// Read a random item name for loot flavor (consumable or junk type)
static bool frpgReadLootItemName(char *dst, size_t dstLen) {
    dst[0] = '\0';
    if (!frpgContentHas(FRPG_CONTENT_ITEMS) || _rpgIdx.itemCount == 0) return false;
    // Pick a random item that is consumable(2) or junk(3)
    File f = bbsExtFS().open(FRPG_ITEMS_PATH, FILE_O_READ);
    if (!f) return false;
    // Try up to 10 times to find a consumable/junk
    for (int attempt = 0; attempt < 10; attempt++) {
        uint16_t idx = (uint16_t)random(_rpgIdx.itemCount);
        uint32_t off = sizeof(RPGItemHeader) + (uint32_t)idx * 16;
        f.seek(off + 7); // item_type at offset 7
        uint8_t itype = 0;
        f.read(&itype, 1);
        if (itype == ITYPE_CONSUMABLE || itype == ITYPE_JUNK) {
            // Read name
            f.seek(off);
            RPGItemEntry item;
            f.read((uint8_t *)&item, 16);
            rpgReadStrTo(f, _rpgIdx.itemPoolOff, item.name_off, dst, dstLen);
            f.close();
            return true;
        }
    }
    f.close();
    return false;
}

// ─── Region helpers ──────────────────────────────────────────────────────────

static uint8_t frpgGetLocRegion(uint8_t locIdx) {
    if (locIdx == 0xFF || !frpgContentHas(FRPG_CONTENT_LOCATIONS)) return 0;
    RPGLocationEntry loc;
    if (!frpgReadLocation(locIdx, loc)) return 0;
    return loc.region_id;
}

// Pick a town in or adjacent to curRegion at or below tier-appropriate danger.
// Falls back to global frpgPickTown if no nearby matches found.
static uint8_t frpgPickTownNearby(uint8_t curRegion, uint8_t maxDanger, uint32_t seed) {
    if (!frpgContentHas(FRPG_CONTENT_LOCATIONS)) return 0xFF;
    // Count matching towns (same or adjacent region)
    File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
    if (!f) return 0xFF;
    uint16_t count = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        uint32_t off = sizeof(RPGLocationHeader) + (uint32_t)i * 18;
        f.seek(off + 5);
        uint8_t lt = 0, dl = 0, rid = 0;
        f.read(&lt, 1); f.read(&dl, 1);
        f.seek(off + 16); f.read(&rid, 1);
        if (lt == 5 && dl <= maxDanger && frpgRegionsAdjacent(curRegion, rid))
            count++;
    }
    if (count == 0) { f.close(); return frpgPickTown(maxDanger, seed); }
    uint16_t pick = (uint16_t)(seed % count);
    uint16_t found = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        uint32_t off = sizeof(RPGLocationHeader) + (uint32_t)i * 18;
        f.seek(off + 5);
        uint8_t lt = 0, dl = 0, rid = 0;
        f.read(&lt, 1); f.read(&dl, 1);
        f.seek(off + 16); f.read(&rid, 1);
        if (lt == 5 && dl <= maxDanger && frpgRegionsAdjacent(curRegion, rid)) {
            if (found == pick) { f.close(); return (uint8_t)i; }
            found++;
        }
    }
    f.close();
    return frpgPickTown(maxDanger, seed);
}

// Pick a non-town adventure in or adjacent to curRegion at given danger.
// Falls back to global frpgPickAdventure if no nearby matches found.
static uint8_t frpgPickAdventureNearby(uint8_t curRegion, uint8_t danger, uint32_t seed) {
    if (!frpgContentHas(FRPG_CONTENT_LOCATIONS)) return 0xFF;
    File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
    if (!f) return 0xFF;
    uint16_t count = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        uint32_t off = sizeof(RPGLocationHeader) + (uint32_t)i * 18;
        f.seek(off + 5);
        uint8_t lt = 0, dl = 0, rid = 0;
        f.read(&lt, 1); f.read(&dl, 1);
        f.seek(off + 16); f.read(&rid, 1);
        if (lt != 5 && dl == danger && frpgRegionsAdjacent(curRegion, rid))
            count++;
    }
    if (count == 0) { f.close(); return frpgPickAdventure(danger, seed); }
    uint16_t pick = (uint16_t)(seed % count);
    uint16_t found = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        uint32_t off = sizeof(RPGLocationHeader) + (uint32_t)i * 18;
        f.seek(off + 5);
        uint8_t lt = 0, dl = 0, rid = 0;
        f.read(&lt, 1); f.read(&dl, 1);
        f.seek(off + 16); f.read(&rid, 1);
        if (lt != 5 && dl == danger && frpgRegionsAdjacent(curRegion, rid)) {
            if (found == pick) { f.close(); return (uint8_t)i; }
            found++;
        }
    }
    f.close();
    return frpgPickAdventure(danger, seed);
}

// ─── Location type helpers ───────────────────────────────────────────────────

static uint8_t frpgGetLocType(uint8_t locIdx) {
    if (locIdx == 0xFF || !frpgContentHas(FRPG_CONTENT_LOCATIONS)) return 1;
    RPGLocationEntry loc;
    if (!frpgReadLocation(locIdx, loc)) return 1;
    return loc.loc_type;
}

static bool frpgIsLocTown(uint8_t locIdx) {
    return frpgGetLocType(locIdx) == 5;
}

static uint8_t frpgGetLocDanger(uint8_t locIdx) {
    if (locIdx == 0xFF || !frpgContentHas(FRPG_CONTENT_LOCATIONS)) return 1;
    RPGLocationEntry loc;
    if (!frpgReadLocation(locIdx, loc)) return 1;
    return loc.danger_level;
}

// Dungeon depth by location type
static uint8_t frpgDungeonMaxDepth(uint8_t locType) {
    static const uint8_t depths[] = {5, 3, 4, 4, 5, 0, 3}; // vault,waste,raider,ghoul,mil,town,cave
    return (locType < 7) ? depths[locType] : 3;
}

// Pick a town location at or below maxDanger. seed for determinism.
static uint8_t frpgPickTown(uint8_t maxDanger, uint32_t seed) {
    if (!frpgContentHas(FRPG_CONTENT_LOCATIONS)) return 0xFF;
    File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
    if (!f) return 0xFF;
    uint16_t townCount = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        f.seek(sizeof(RPGLocationHeader) + (uint32_t)i * 18 + 5);
        uint8_t lt = 0; f.read(&lt, 1);
        uint8_t dl = 0; f.read(&dl, 1);
        if (lt == 5 && dl <= maxDanger) townCount++;
    }
    if (townCount == 0) { f.close(); return 0xFF; }
    uint16_t pick = (uint16_t)(seed % townCount);
    uint16_t found = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        f.seek(sizeof(RPGLocationHeader) + (uint32_t)i * 18 + 5);
        uint8_t lt = 0; f.read(&lt, 1);
        uint8_t dl = 0; f.read(&dl, 1);
        if (lt == 5 && dl <= maxDanger) {
            if (found == pick) { f.close(); return (uint8_t)i; }
            found++;
        }
    }
    f.close();
    return 0xFF;
}

// Pick a non-town adventure location at given danger.
static uint8_t frpgPickAdventure(uint8_t danger, uint32_t seed) {
    if (!frpgContentHas(FRPG_CONTENT_LOCATIONS)) return 0xFF;
    File f = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
    if (!f) return 0xFF;
    uint16_t count = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        f.seek(sizeof(RPGLocationHeader) + (uint32_t)i * 18 + 5);
        uint8_t lt = 0; f.read(&lt, 1);
        uint8_t dl = 0; f.read(&dl, 1);
        if (lt != 5 && dl == danger) count++;
    }
    if (count == 0) { f.close(); return 0xFF; }
    uint16_t pick = (uint16_t)(seed % count);
    uint16_t found = 0;
    for (uint16_t i = 0; i < _rpgIdx.locCount; i++) {
        f.seek(sizeof(RPGLocationHeader) + (uint32_t)i * 18 + 5);
        uint8_t lt = 0; f.read(&lt, 1);
        uint8_t dl = 0; f.read(&dl, 1);
        if (lt != 5 && dl == danger) {
            if (found == pick) { f.close(); return (uint8_t)i; }
            found++;
        }
    }
    f.close();
    return 0xFF;
}

// Pick an enemy from a location's enemy list by name matching.
static uint16_t frpgPickLocationEnemy(uint8_t locIdx, bool bossPick) {
    if (locIdx == 0xFF || !frpgContentHas(FRPG_CONTENT_LOCATIONS) || !frpgContentHas(FRPG_CONTENT_ENEMIES))
        return frpgPickTierEnemy(1);
    RPGLocationEntry loc;
    if (!frpgReadLocation(locIdx, loc) || loc.enemy_count == 0)
        return frpgPickTierEnemy(loc.danger_level);
    uint8_t pickIdx = bossPick ? (loc.enemy_count - 1) : (uint8_t)(random(loc.enemy_count));
    uint16_t targetNameOff = loc.enemies[pickIdx];
    if (targetNameOff == 0xFFFF) return frpgPickTierEnemy(loc.danger_level);
    // Read target name from location pool
    char targetName[24] = {0};
    {
        File lf = bbsExtFS().open(FRPG_LOCS_PATH, FILE_O_READ);
        if (!lf) return frpgPickTierEnemy(loc.danger_level);
        rpgReadStrTo(lf, _rpgIdx.locPoolOff, targetNameOff, targetName, sizeof(targetName));
        lf.close();
    }
    if (!targetName[0]) return frpgPickTierEnemy(loc.danger_level);
    // Scan enemies.bin for name match
    File ef = bbsExtFS().open(FRPG_ENEMIES_PATH, FILE_O_READ);
    if (!ef) return frpgPickTierEnemy(loc.danger_level);
    for (uint16_t i = 0; i < _rpgIdx.enemyCount; i++) {
        ef.seek(sizeof(RPGEnemyHeader) + (uint32_t)i * 30);
        uint16_t nameOff = 0;
        ef.read((uint8_t *)&nameOff, 2);
        char eName[24] = {0};
        rpgReadStrTo(ef, _rpgIdx.enemyPoolOff, nameOff, eName, sizeof(eName));
        if (strcmp(eName, targetName) == 0) { ef.close(); return i; }
    }
    ef.close();
    return frpgPickTierEnemy(loc.danger_level);
}
