#pragma once
// MudWorld.h — persistent world data for TinyMUD on Meshtastic
// All I/O uses FSCom (LittleFS). Never call from constructor on nRF52.

#include "DebugConfiguration.h"
#include "FSCommon.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

#define MUD_DIR              "/mud"
#define MUD_ROOMS_DIR        "/mud/rooms"
#define MUD_PLAYERS_DIR      "/mud/players"
#define MUD_ITEMS_DIR        "/mud/items"
#define MUD_META_PATH        "/mud/meta.bin"
#define MUD_ITEMS_META_PATH  "/mud/items/meta.bin"
#define MUD_START_ROOM       1
#define MUD_MAX_EXITS        6
#define MUD_MAX_ITEMS_SCAN   24
#define MUD_FLAG_ADMIN       0x01

// ─── On-flash data structures ───────────────────────────────────────────────

struct __attribute__((packed)) MudRoomExit {
    char     dir[4];   // "n","s","e","w","u","d","ne","nw","se","sw"
    uint16_t toId;     // destination room id
};

struct __attribute__((packed)) MudRoom {
    uint16_t    id;
    char        name[28];
    char        desc[100];
    MudRoomExit exits[MUD_MAX_EXITS];
    uint8_t     numExits;
    uint8_t     flags;
};
// sizeof: 2+28+100+36+1+1 = 168 bytes

struct __attribute__((packed)) MudPlayerData {
    uint32_t nodeNum;
    uint16_t roomId;
    char     name[16];
    uint8_t  flags;
    uint16_t score;
};
// sizeof: 4+2+16+1+2 = 25 bytes

struct __attribute__((packed)) MudItem {
    uint16_t id;
    char     name[20];
    char     desc[60];
    uint16_t roomId;       // 0 = held by a player
    uint32_t holderNode;   // valid when roomId == 0
    uint8_t  flags;
};
// sizeof: 2+20+60+2+4+1 = 89 bytes

// ─── World storage class ────────────────────────────────────────────────────

class MudWorld {
  public:
    // Call once, lazily, on first message. Safe on nRF52 (not from constructor).
    bool init() {
        if (initialized_) return true;
        if (!FSCom.exists(MUD_DIR))         FSCom.mkdir(MUD_DIR);
        if (!FSCom.exists(MUD_ROOMS_DIR))   FSCom.mkdir(MUD_ROOMS_DIR);
        if (!FSCom.exists(MUD_PLAYERS_DIR)) FSCom.mkdir(MUD_PLAYERS_DIR);
        if (!FSCom.exists(MUD_ITEMS_DIR))   FSCom.mkdir(MUD_ITEMS_DIR);

        if (FSCom.exists(MUD_META_PATH)) {
            loadMeta();
        } else {
            writeDefaultWorld();
        }
        initialized_ = true;
        LOG_DEBUG("[MUD] world init: %u rooms, nextRoom=%u, nextItem=%u\n",
                  numRooms_, nextRoomId_, nextItemId_);
        return numRooms_ > 0;
    }

    // ── Room I/O ──────────────────────────────────────────────────────────

    bool loadRoom(uint16_t id, MudRoom &out) const {
        char path[48]; roomPath(id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_READ);
        if (!f) return false;
        bool ok = (f.read((uint8_t *)&out, sizeof(MudRoom)) == sizeof(MudRoom));
        f.close();
        return ok;
    }

    bool saveRoom(const MudRoom &r) const {
        char path[48]; roomPath(r.id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_WRITE);
        if (!f) return false;
        f.write((const uint8_t *)&r, sizeof(MudRoom));
        f.close();
        return true;
    }

    // Allocate a new room ID and persist meta.
    uint16_t allocRoomId() {
        uint16_t id = nextRoomId_++;
        numRooms_++;
        saveMeta();
        return id;
    }

    // ── Player I/O ────────────────────────────────────────────────────────

    bool loadPlayer(uint32_t nodeNum, MudPlayerData &out) const {
        char path[56]; playerPath(nodeNum, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_READ);
        if (!f) return false;
        bool ok = (f.read((uint8_t *)&out, sizeof(MudPlayerData)) == sizeof(MudPlayerData));
        f.close();
        return ok;
    }

    bool savePlayer(const MudPlayerData &data) const {
        char path[56]; playerPath(data.nodeNum, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_WRITE);
        if (!f) return false;
        f.write((const uint8_t *)&data, sizeof(MudPlayerData));
        f.close();
        return true;
    }

    // ── Item I/O ──────────────────────────────────────────────────────────

    bool loadItem(uint16_t id, MudItem &out) const {
        char path[56]; itemPath(id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_READ);
        if (!f) return false;
        bool ok = (f.read((uint8_t *)&out, sizeof(MudItem)) == sizeof(MudItem));
        f.close();
        return ok;
    }

    bool saveItem(const MudItem &item) const {
        char path[56]; itemPath(item.id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_WRITE);
        if (!f) return false;
        f.write((const uint8_t *)&item, sizeof(MudItem));
        f.close();
        return true;
    }

    bool deleteItem(uint16_t id) const {
        char path[56]; itemPath(id, path, sizeof(path));
        return FSCom.remove(path);
    }

    // Allocate a new item ID and persist meta.
    uint16_t allocItemId() {
        uint16_t id = nextItemId_++;
        saveItemMeta();
        return id;
    }

    // Find item IDs in a room. Returns count written to out[].
    int findItemsInRoom(uint16_t roomId, uint16_t *out, int max) const {
        int found = 0;
        for (uint16_t id = 1; id < nextItemId_ && found < max && id <= MUD_MAX_ITEMS_SCAN; id++) {
            MudItem item;
            if (!loadItem(id, item)) continue;
            if (item.roomId == roomId) out[found++] = id;
        }
        return found;
    }

    // Find item IDs held by a player.
    int findItemsOnPlayer(uint32_t nodeNum, uint16_t *out, int max) const {
        int found = 0;
        for (uint16_t id = 1; id < nextItemId_ && found < max && id <= MUD_MAX_ITEMS_SCAN; id++) {
            MudItem item;
            if (!loadItem(id, item)) continue;
            if (item.roomId == 0 && item.holderNode == nodeNum) out[found++] = id;
        }
        return found;
    }

    uint8_t  numRooms()   const { return numRooms_; }
    uint16_t nextRoomId() const { return nextRoomId_; }
    uint16_t nextItemId() const { return nextItemId_; }

  private:
    uint8_t  numRooms_    = 0;
    uint16_t nextRoomId_  = 0;   // 0 = not yet loaded
    uint16_t nextItemId_  = 1;
    bool     initialized_ = false;

    void roomPath(uint16_t id, char *buf, size_t n) const {
        snprintf(buf, n, MUD_ROOMS_DIR "/%04u.bin", id);
    }
    void playerPath(uint32_t nodeNum, char *buf, size_t n) const {
        snprintf(buf, n, MUD_PLAYERS_DIR "/%08" PRIx32 ".bin", nodeNum);
    }
    void itemPath(uint16_t id, char *buf, size_t n) const {
        snprintf(buf, n, MUD_ITEMS_DIR "/%04u.bin", id);
    }

    // Meta file layout (MUD_META_PATH):
    //   byte 0     : numRooms  (uint8_t)
    //   bytes 1-2  : nextRoomId (uint16_t LE); 0 = use numRooms+1 (old format)
    void loadMeta() {
        File f = FSCom.open(MUD_META_PATH, FILE_O_READ);
        if (!f) return;
        f.read(&numRooms_, 1);
        uint16_t nrid = 0;
        f.read((uint8_t *)&nrid, 2);
        f.close();
        nextRoomId_ = (nrid > 0 && nrid <= 255) ? nrid : (uint16_t)numRooms_ + 1;
        loadItemMeta();
    }

    void saveMeta() const {
        File f = FSCom.open(MUD_META_PATH, FILE_O_WRITE);
        if (!f) return;
        f.write(&numRooms_, 1);
        f.write((const uint8_t *)&nextRoomId_, 2);
        f.close();
    }

    void loadItemMeta() {
        File f = FSCom.open(MUD_ITEMS_META_PATH, FILE_O_READ);
        if (!f) { nextItemId_ = 1; return; }
        uint16_t nid = 0;
        if (f.read((uint8_t *)&nid, 2) == 2 && nid >= 1)
            nextItemId_ = nid;
        else
            nextItemId_ = 1;
        f.close();
    }

    void saveItemMeta() const {
        File f = FSCom.open(MUD_ITEMS_META_PATH, FILE_O_WRITE);
        if (!f) return;
        f.write((const uint8_t *)&nextItemId_, 2);
        f.close();
    }

    // ── Default 8-room starter world ──────────────────────────────────────
    void writeDefaultWorld() {
        struct RDef {
            uint16_t    id;
            const char *name;
            const char *desc;
            uint8_t     numExits;
            MudRoomExit exits[MUD_MAX_EXITS];
        };
        static const RDef DEFS[] = {
            {1, "Town Square",
             "A dusty square at the heart of a small settlement. A notice board stands nearby.",
             3, {{"n", 2}, {"e", 3}, {"s", 4}}},

            {2, "Market",
             "Empty stalls line this plaza. A faint smell of spices lingers. South leads to the square.",
             2, {{"s", 1}, {"w", 5}}},

            {3, "The Rusty Axe Inn",
             "A warm common room with a crackling hearth. Travelers swap tales here. West exits to the square.",
             2, {{"w", 1}, {"u", 6}}},

            {4, "South Road",
             "A winding road out of town. The gate is north. Dark forest lies east.",
             2, {{"n", 1}, {"e", 7}}},

            {5, "Alchemist's Shop",
             "Shelves of odd bottles and dried herbs crowd the walls. The market is east.",
             1, {{"e", 2}}},

            {6, "Inn Upper Floor",
             "A narrow hallway of closed doors. A window overlooks the square. Down to the common room.",
             1, {{"d", 3}}},

            {7, "Forest Edge",
             "Tall oaks press close here. The forest stretches east. Town road lies west.",
             2, {{"w", 4}, {"e", 8}}},

            {8, "Deep Forest",
             "Pale light pierces the canopy. Ancient trees creak. Something watches from the shadows.",
             1, {{"w", 7}}},
        };

        uint8_t n = sizeof(DEFS) / sizeof(DEFS[0]);
        for (uint8_t i = 0; i < n; i++) {
            MudRoom r;
            memset(&r, 0, sizeof(r));
            r.id = DEFS[i].id;
            strncpy(r.name, DEFS[i].name, sizeof(r.name) - 1);
            strncpy(r.desc, DEFS[i].desc, sizeof(r.desc) - 1);
            r.numExits = DEFS[i].numExits;
            for (uint8_t j = 0; j < r.numExits; j++) r.exits[j] = DEFS[i].exits[j];
            saveRoom(r);
        }
        numRooms_   = n;
        nextRoomId_ = (uint16_t)n + 1;
        saveMeta();

        writeDefaultItems();
        LOG_DEBUG("[MUD] default world written: %u rooms\n", numRooms_);
    }

    // ── Default starter items ─────────────────────────────────────────────
    void writeDefaultItems() {
        struct IDef { const char *name; const char *desc; uint16_t roomId; };
        static const IDef ITEMS[] = {
            {"old map",     "A tattered map of the region. Hard to read.",           1},
            {"rusty sword", "A notched blade, still serviceable in a pinch.",        7},
            {"strange vial","A small bottle of shimmering liquid. Label worn off.",  5},
        };
        nextItemId_ = 1;
        uint8_t n = sizeof(ITEMS) / sizeof(ITEMS[0]);
        for (uint8_t i = 0; i < n; i++) {
            MudItem item;
            memset(&item, 0, sizeof(item));
            item.id     = nextItemId_++;
            item.roomId = ITEMS[i].roomId;
            strncpy(item.name, ITEMS[i].name, sizeof(item.name) - 1);
            strncpy(item.desc, ITEMS[i].desc, sizeof(item.desc) - 1);
            saveItem(item);
        }
        saveItemMeta();
        LOG_DEBUG("[MUD] default items written: %u\n", n);
    }
};
