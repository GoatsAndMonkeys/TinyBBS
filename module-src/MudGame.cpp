// MudGame.cpp — TinyMUD game logic for Meshtastic/nRF52
// nRF52 rules: no FS ops in constructor, no printf/fflush, no delay()

#include "MudGame.h"
#include "Channels.h"
#include "DebugConfiguration.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Router.h"
#include <cctype>
#include <cstdio>
#include <cstring>

// ── Constructor ────────────────────────────────────────────────────────────

MudGame::MudGame() {
    memset(sessions_, 0, sizeof(sessions_));
}

// ── World init (lazy) ──────────────────────────────────────────────────────

bool MudGame::ensureWorld() {
    if (worldReady_) return true;
    worldReady_ = world_.init();
    return worldReady_;
}

// ── Session management ─────────────────────────────────────────────────────

MudSession *MudGame::getSession(uint32_t nodeNum) {
    for (int i = 0; i < MUD_MAX_SESSIONS; i++)
        if (sessions_[i].active && sessions_[i].nodeNum == nodeNum)
            return &sessions_[i];
    return nullptr;
}

MudSession *MudGame::createSession(uint32_t nodeNum, const char *name, uint16_t roomId) {
    MudSession *slot = getSession(nodeNum);
    if (!slot) {
        for (int i = 0; i < MUD_MAX_SESSIONS; i++) {
            if (!sessions_[i].active) { slot = &sessions_[i]; break; }
        }
    }
    if (!slot) {
        // Evict oldest session
        uint32_t oldest = sessions_[0].lastActivity;
        slot = &sessions_[0];
        for (int i = 1; i < MUD_MAX_SESSIONS; i++) {
            if (sessions_[i].lastActivity < oldest) {
                oldest = sessions_[i].lastActivity;
                slot = &sessions_[i];
            }
        }
        persistSession(*slot);
    }
    memset(slot, 0, sizeof(MudSession));
    slot->nodeNum      = nodeNum;
    slot->roomId       = roomId;
    slot->lastActivity = (uint32_t)getTime();
    slot->active       = true;
    strncpy(slot->name, name ? name : "Wanderer", sizeof(slot->name) - 1);
    return slot;
}

void MudGame::expireSessions(uint32_t now) {
    for (int i = 0; i < MUD_MAX_SESSIONS; i++) {
        if (!sessions_[i].active) continue;
        if (now > sessions_[i].lastActivity &&
            (now - sessions_[i].lastActivity) > MUD_SESSION_TIMEOUT) {
            LOG_DEBUG("[MUD] expiring session for node 0x%08x\n", sessions_[i].nodeNum);
            persistSession(sessions_[i]);
            sessions_[i].active = false;
        }
    }
}

void MudGame::persistSession(const MudSession &s) {
    if (!worldReady_ || !s.active) return;
    MudPlayerData pd;
    memset(&pd, 0, sizeof(pd));
    pd.nodeNum = s.nodeNum;
    pd.roomId  = s.roomId;
    pd.flags   = s.flags;
    strncpy(pd.name, s.name, sizeof(pd.name) - 1);
    world_.savePlayer(pd);
}

// ── Public API ─────────────────────────────────────────────────────────────

void MudGame::enter(uint32_t nodeNum, const char *shortName,
                    char *reply, size_t replyLen) {
    if (!ensureWorld()) {
        snprintf(reply, replyLen, "[MUD] World unavailable.");
        return;
    }
    expireSessions((uint32_t)getTime());

    MudPlayerData pd;
    memset(&pd, 0, sizeof(pd));
    uint16_t startRoom = MUD_START_ROOM;
    char name[16] = {0};
    bool hadSave = world_.loadPlayer(nodeNum, pd) && pd.roomId >= 1;

    if (hadSave) {
        startRoom = pd.roomId;
        strncpy(name, pd.name, sizeof(name) - 1);
    }
    if (name[0] == '\0') {
        strncpy(name, shortName ? shortName : "Wanderer", sizeof(name) - 1);
    }

    MudSession *s = createSession(nodeNum, name, startRoom);
    if (!s) {
        snprintf(reply, replyLen, "[MUD] No session slots free.");
        return;
    }
    // Restore saved flags (e.g. admin status)
    if (hadSave) s->flags = pd.flags;

    char announce[64];
    snprintf(announce, sizeof(announce), "%s has entered the realm.", s->name);
    broadcastRoom(s->roomId, nodeNum, announce);

    MudRoom room;
    char lookBuf[MUD_REPLY_MAX] = {0};
    if (world_.loadRoom(s->roomId, room)) {
        buildLook(room, *s, lookBuf, sizeof(lookBuf));
    }
    snprintf(reply, replyLen, "Welcome to TinyMUD, %s!\n%s", s->name, lookBuf);
}

bool MudGame::handle(const meshtastic_MeshPacket &mp, const char *text,
                     char *reply, size_t replyLen) {
    if (!ensureWorld()) {
        snprintf(reply, replyLen, "[MUD] World error.");
        return false;
    }
    if (!text || text[0] == '\0') {
        MudSession *s = getSession(mp.from);
        if (s) return cmdLook(*s, reply, replyLen);
        snprintf(reply, replyLen, "Type 'help' for commands.");
        return false;
    }

    uint32_t now = (uint32_t)getTime();
    expireSessions(now);

    MudSession *s = getSession(mp.from);
    if (!s) {
        // Session expired — re-enter
        const char *sname = nullptr;
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(mp.from);
        if (node && node->has_user && node->user.short_name[0])
            sname = node->user.short_name;
        enter(mp.from, sname, reply, replyLen);
        return false;
    }
    s->lastActivity = now;

    // Strip leading whitespace
    while (*text == ' ' || *text == '\t') text++;

    // "say" shorthand: lines starting with "
    if (text[0] == '"') {
        return cmdSay(*s, text + 1);
    }

    // Extract first word as command, rest as argument
    char cmd[16] = {0};
    const char *arg = "";
    const char *sp = strchr(text, ' ');
    if (sp) {
        size_t cmdLen = sp - text;
        if (cmdLen >= sizeof(cmd)) cmdLen = sizeof(cmd) - 1;
        memcpy(cmd, text, cmdLen);
        arg = sp + 1;
        while (*arg == ' ') arg++;
    } else {
        strncpy(cmd, text, sizeof(cmd) - 1);
    }

    // Lowercase the command
    for (char *p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);

    // ── Admin commands (@prefix) ─────────────────────────────────────────
    if (cmd[0] == '@') {
        return cmdAdminCmd(*s, cmd + 1, arg, reply, replyLen);
    }

    // ── Movement ─────────────────────────────────────────────────────────
    if (strcmp(cmd, "go") == 0) {
        return cmdGo(*s, arg, reply, replyLen);

    } else if (strcmp(cmd, "n")  == 0 || strcmp(cmd, "north") == 0 ||
               strcmp(cmd, "s")  == 0 || strcmp(cmd, "south") == 0 ||
               strcmp(cmd, "e")  == 0 || strcmp(cmd, "east")  == 0 ||
               strcmp(cmd, "w")  == 0 || strcmp(cmd, "west")  == 0 ||
               strcmp(cmd, "u")  == 0 || strcmp(cmd, "up")    == 0 ||
               strcmp(cmd, "d")  == 0 || strcmp(cmd, "down")  == 0 ||
               strcmp(cmd, "ne") == 0 || strcmp(cmd, "nw")    == 0 ||
               strcmp(cmd, "se") == 0 || strcmp(cmd, "sw")    == 0) {
        return cmdGo(*s, cmd, reply, replyLen);

    // ── Look / examine ────────────────────────────────────────────────────
    } else if (strcmp(cmd, "look") == 0 || strcmp(cmd, "l") == 0) {
        return cmdLook(*s, reply, replyLen);

    } else if (strcmp(cmd, "examine") == 0 || strcmp(cmd, "ex") == 0) {
        return cmdExamine(*s, arg, reply, replyLen);

    // ── Items ─────────────────────────────────────────────────────────────
    } else if (strcmp(cmd, "get")  == 0 || strcmp(cmd, "take") == 0) {
        return cmdTake(*s, arg, reply, replyLen);

    } else if (strcmp(cmd, "drop") == 0) {
        return cmdDrop(*s, arg, reply, replyLen);

    } else if (strcmp(cmd, "inv")  == 0 || strcmp(cmd, "i") == 0 ||
               strcmp(cmd, "inventory") == 0) {
        return cmdInventory(*s, reply, replyLen);

    // ── Social / info ─────────────────────────────────────────────────────
    } else if (strcmp(cmd, "say") == 0) {
        return cmdSay(*s, arg);

    } else if (strcmp(cmd, "who") == 0) {
        return cmdWho(reply, replyLen);

    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0 ||
               strcmp(cmd, "h") == 0) {
        return cmdHelp(reply, replyLen);

    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0 ||
               strcmp(cmd, "exit") == 0 || strcmp(cmd, "x") == 0) {
        return cmdQuit(*s, reply, replyLen);

    } else {
        snprintf(reply, replyLen,
                 "Unknown command '%s'.\nType 'help' for a list.", cmd);
    }
    return false;
}

// ── Command implementations ────────────────────────────────────────────────

bool MudGame::cmdLook(MudSession &s, char *reply, size_t len) {
    MudRoom room;
    if (!world_.loadRoom(s.roomId, room)) {
        snprintf(reply, len, "You are in a dark void. (Room %u missing)", s.roomId);
        return false;
    }
    buildLook(room, s, reply, len);
    return false;
}

bool MudGame::cmdGo(MudSession &s, const char *dir, char *reply, size_t len) {
    if (!dir || dir[0] == '\0') {
        snprintf(reply, len, "Go where?");
        return false;
    }

    char d[4] = {0};
    strncpy(d, dir, 3);
    for (char *p = d; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strcmp(d, "north") == 0) strcpy(d, "n");
    else if (strcmp(d, "south") == 0) strcpy(d, "s");
    else if (strcmp(d, "east")  == 0) strcpy(d, "e");
    else if (strcmp(d, "west")  == 0) strcpy(d, "w");
    else if (strcmp(d, "up")    == 0) strcpy(d, "u");
    else if (strcmp(d, "down")  == 0) strcpy(d, "d");

    MudRoom room;
    if (!world_.loadRoom(s.roomId, room)) {
        snprintf(reply, len, "Lost in void. (Room %u missing)", s.roomId);
        return false;
    }

    uint16_t destId = 0;
    for (int i = 0; i < room.numExits; i++) {
        if (strcmp(room.exits[i].dir, d) == 0) {
            destId = room.exits[i].toId;
            break;
        }
    }
    if (destId == 0) {
        snprintf(reply, len, "You can't go %s from here.", d);
        return false;
    }

    MudRoom dest;
    if (!world_.loadRoom(destId, dest)) {
        snprintf(reply, len, "That way is blocked. (Room %u missing)", destId);
        return false;
    }

    char depart[64];
    snprintf(depart, sizeof(depart), "%s heads %s.", s.name, d);
    broadcastRoom(s.roomId, s.nodeNum, depart);

    s.roomId = destId;

    const char *fromDir = oppositeDir(d);
    char arrive[64];
    if (fromDir[0])
        snprintf(arrive, sizeof(arrive), "%s arrives from the %s.", s.name, fromDir);
    else
        snprintf(arrive, sizeof(arrive), "%s arrives.", s.name);
    broadcastRoom(destId, s.nodeNum, arrive);

    persistSession(s);

    buildLook(dest, s, reply, len);
    return false;
}

bool MudGame::cmdSay(MudSession &s, const char *msg) {
    if (!msg || msg[0] == '\0') return false;
    char broadcast[MUD_REPLY_MAX];
    snprintf(broadcast, sizeof(broadcast), "%s says: \"%s\"", s.name, msg);
    broadcastRoom(s.roomId, s.nodeNum, broadcast);
    char self[MUD_REPLY_MAX];
    snprintf(self, sizeof(self), "You say: \"%s\"", msg);
    sendDM(s.nodeNum, self);
    return false;
}

bool MudGame::cmdWho(char *reply, size_t len) {
    char buf[MUD_REPLY_MAX] = "[MUD] Online:\n";
    bool found = false;
    for (int i = 0; i < MUD_MAX_SESSIONS; i++) {
        if (!sessions_[i].active) continue;
        found = true;
        MudRoom room;
        char line[48];
        if (world_.loadRoom(sessions_[i].roomId, room))
            snprintf(line, sizeof(line), "- %s (%s)\n", sessions_[i].name, room.name);
        else
            snprintf(line, sizeof(line), "- %s (room %u)\n", sessions_[i].name, sessions_[i].roomId);
        strncat(buf, line, len - strlen(buf) - 1);
    }
    if (!found) strncat(buf, "(nobody)", len - strlen(buf) - 1);
    snprintf(reply, len, "%s", buf);
    return false;
}

bool MudGame::cmdHelp(char *reply, size_t len) {
    snprintf(reply, len,
             "[TinyMUD]\n"
             "look(l)  go <dir>\n"
             "n s e w u d\n"
             "say(\"msg)  who\n"
             "get/drop/inv\n"
             "examine(ex) <item>\n"
             "quit(q)");
    return false;
}

bool MudGame::cmdQuit(MudSession &s, char *reply, size_t len) {
    char farewell[48];
    snprintf(farewell, sizeof(farewell), "%s has left the realm.", s.name);
    broadcastRoom(s.roomId, s.nodeNum, farewell);
    persistSession(s);
    s.active = false;
    snprintf(reply, len, "Farewell, %s. Returning to BBS...", s.name);
    return true;
}

// ── Item commands ──────────────────────────────────────────────────────────


bool MudGame::cmdInventory(MudSession &s, char *reply, size_t len) {
    uint16_t ids[8];
    int n = world_.findItemsOnPlayer(s.nodeNum, ids, 8);
    if (n == 0) {
        snprintf(reply, len, "You're carrying nothing.");
        return false;
    }
    char buf[MUD_REPLY_MAX] = "Carrying:";
    for (int i = 0; i < n; i++) {
        MudItem item;
        if (!world_.loadItem(ids[i], item)) continue;
        strncat(buf, "\n- ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, item.name, sizeof(buf) - strlen(buf) - 1);
    }
    snprintf(reply, len, "%s", buf);
    return false;
}

bool MudGame::cmdTake(MudSession &s, const char *name, char *reply, size_t len) {
    if (!name || !name[0]) {
        snprintf(reply, len, "Take what?");
        return false;
    }
    uint16_t ids[8];
    int n = world_.findItemsInRoom(s.roomId, ids, 8);
    for (int i = 0; i < n; i++) {
        MudItem item;
        if (!world_.loadItem(ids[i], item)) continue;
        if (!prefixMatchCI(item.name, name)) continue;
        item.roomId     = 0;
        item.holderNode = s.nodeNum;
        world_.saveItem(item);
        snprintf(reply, len, "You take the %s.", item.name);
        return false;
    }
    snprintf(reply, len, "You don't see '%s' here.", name);
    return false;
}

bool MudGame::cmdDrop(MudSession &s, const char *name, char *reply, size_t len) {
    if (!name || !name[0]) {
        snprintf(reply, len, "Drop what?");
        return false;
    }
    uint16_t ids[8];
    int n = world_.findItemsOnPlayer(s.nodeNum, ids, 8);
    for (int i = 0; i < n; i++) {
        MudItem item;
        if (!world_.loadItem(ids[i], item)) continue;
        if (!prefixMatchCI(item.name, name)) continue;
        item.roomId     = s.roomId;
        item.holderNode = 0;
        world_.saveItem(item);
        snprintf(reply, len, "You drop the %s.", item.name);
        return false;
    }
    snprintf(reply, len, "You're not carrying '%s'.", name);
    return false;
}

bool MudGame::cmdExamine(MudSession &s, const char *name, char *reply, size_t len) {
    if (!name || !name[0]) {
        snprintf(reply, len, "Examine what?");
        return false;
    }
    uint16_t ids[8];
    int n;

    // Check inventory first
    n = world_.findItemsOnPlayer(s.nodeNum, ids, 8);
    for (int i = 0; i < n; i++) {
        MudItem item;
        if (!world_.loadItem(ids[i], item)) continue;
        if (prefixMatchCI(item.name, name)) {
            snprintf(reply, len, "%s: %s", item.name, item.desc);
            return false;
        }
    }
    // Then room
    n = world_.findItemsInRoom(s.roomId, ids, 8);
    for (int i = 0; i < n; i++) {
        MudItem item;
        if (!world_.loadItem(ids[i], item)) continue;
        if (prefixMatchCI(item.name, name)) {
            snprintf(reply, len, "%s: %s", item.name, item.desc);
            return false;
        }
    }
    snprintf(reply, len, "You see no '%s'.", name);
    return false;
}

// ── Admin commands ─────────────────────────────────────────────────────────

bool MudGame::cmdAdminCmd(MudSession &s, const char *subcmd, const char *arg,
                          char *reply, size_t len) {
    // @admin — claim admin status (no guard; first-use claim for small mesh)
    if (strcmp(subcmd, "admin") == 0) {
        s.flags |= MUD_FLAG_ADMIN;
        persistSession(s);
        snprintf(reply, len, "Admin granted to %s.", s.name);
        return false;
    }

    if (!(s.flags & MUD_FLAG_ADMIN)) {
        snprintf(reply, len, "Permission denied. Use '@admin' first.");
        return false;
    }

    if (strcmp(subcmd, "desc") == 0) {
        // @desc <new description>
        if (!arg[0]) { snprintf(reply, len, "Usage: @desc <text>"); return false; }
        MudRoom room;
        if (!world_.loadRoom(s.roomId, room)) {
            snprintf(reply, len, "Room load error."); return false;
        }
        strncpy(room.desc, arg, sizeof(room.desc) - 1);
        room.desc[sizeof(room.desc) - 1] = '\0';
        world_.saveRoom(room);
        snprintf(reply, len, "Room description updated.");

    } else if (strcmp(subcmd, "name") == 0) {
        // @name <new room name>
        if (!arg[0]) { snprintf(reply, len, "Usage: @name <text>"); return false; }
        MudRoom room;
        if (!world_.loadRoom(s.roomId, room)) {
            snprintf(reply, len, "Room load error."); return false;
        }
        strncpy(room.name, arg, sizeof(room.name) - 1);
        room.name[sizeof(room.name) - 1] = '\0';
        world_.saveRoom(room);
        snprintf(reply, len, "Room renamed to '%s'.", room.name);

    } else if (strcmp(subcmd, "dig") == 0) {
        // @dig <dir> <room name>
        char dir[4] = {0};
        const char *rname = "";
        const char *dsp = strchr(arg, ' ');
        if (dsp) {
            size_t dl = dsp - arg < 3 ? (size_t)(dsp - arg) : 3;
            memcpy(dir, arg, dl);
            rname = dsp + 1;
            while (*rname == ' ') rname++;
        } else {
            strncpy(dir, arg, 3);
        }
        for (char *p = dir; *p; p++) *p = (char)tolower((unsigned char)*p);

        if (!dir[0] || !rname[0]) {
            snprintf(reply, len, "Usage: @dig <dir> <name>"); return false;
        }
        MudRoom cur;
        if (!world_.loadRoom(s.roomId, cur)) {
            snprintf(reply, len, "Room load error."); return false;
        }
        for (int i = 0; i < cur.numExits; i++) {
            if (strcmp(cur.exits[i].dir, dir) == 0) {
                snprintf(reply, len, "Exit '%s' already exists.", dir); return false;
            }
        }
        if (cur.numExits >= MUD_MAX_EXITS) {
            snprintf(reply, len, "Room has max exits (%d).", MUD_MAX_EXITS); return false;
        }

        uint16_t newId = world_.allocRoomId();
        MudRoom nr;
        memset(&nr, 0, sizeof(nr));
        nr.id = newId;
        strncpy(nr.name, rname, sizeof(nr.name) - 1);
        strncpy(nr.desc, "An empty room.", sizeof(nr.desc) - 1);
        const char *rev = reverseDir(dir);
        if (rev[0]) {
            strncpy(nr.exits[0].dir, rev, sizeof(nr.exits[0].dir) - 1);
            nr.exits[0].toId = s.roomId;
            nr.numExits = 1;
        }
        world_.saveRoom(nr);

        cur.exits[cur.numExits].toId = newId;
        strncpy(cur.exits[cur.numExits].dir, dir, sizeof(cur.exits[0].dir) - 1);
        cur.numExits++;
        world_.saveRoom(cur);
        snprintf(reply, len, "Dug room #%u '%s' to the %s.", newId, rname, dir);

    } else if (strcmp(subcmd, "link") == 0) {
        // @link <dir> <roomid>  — one-directional exit from current room
        char dir[4] = {0};
        const char *ridStr = "";
        const char *dsp = strchr(arg, ' ');
        if (!dsp) {
            snprintf(reply, len, "Usage: @link <dir> <roomid>"); return false;
        }
        size_t dl = dsp - arg < 3 ? (size_t)(dsp - arg) : 3;
        memcpy(dir, arg, dl);
        for (char *p = dir; *p; p++) *p = (char)tolower((unsigned char)*p);
        ridStr = dsp + 1;
        while (*ridStr == ' ') ridStr++;
        uint16_t destId = (uint16_t)atoi(ridStr);
        if (destId == 0) {
            snprintf(reply, len, "Invalid room id."); return false;
        }
        MudRoom cur;
        if (!world_.loadRoom(s.roomId, cur)) {
            snprintf(reply, len, "Room load error."); return false;
        }
        if (cur.numExits >= MUD_MAX_EXITS) {
            snprintf(reply, len, "Room has max exits (%d).", MUD_MAX_EXITS); return false;
        }
        cur.exits[cur.numExits].toId = destId;
        strncpy(cur.exits[cur.numExits].dir, dir, sizeof(cur.exits[0].dir) - 1);
        cur.numExits++;
        world_.saveRoom(cur);
        snprintf(reply, len, "Linked %s -> room %u.", dir, destId);

    } else if (strcmp(subcmd, "create") == 0) {
        // @create <item name>
        if (!arg[0]) { snprintf(reply, len, "Usage: @create <name>"); return false; }
        MudItem item;
        memset(&item, 0, sizeof(item));
        item.id     = world_.allocItemId();
        item.roomId = s.roomId;
        strncpy(item.name, arg, sizeof(item.name) - 1);
        strncpy(item.desc, "An unremarkable object.", sizeof(item.desc) - 1);
        world_.saveItem(item);
        snprintf(reply, len, "Created '%s' here (id %u).", item.name, item.id);

    } else if (strcmp(subcmd, "idesc") == 0) {
        // @idesc <name-prefix> <new description>
        const char *dsp = strchr(arg, ' ');
        if (!dsp) { snprintf(reply, len, "Usage: @idesc <name> <desc>"); return false; }
        char prefix[20] = {0};
        size_t pl = dsp - arg < 19 ? (size_t)(dsp - arg) : 19;
        memcpy(prefix, arg, pl);
        const char *newdesc = dsp + 1;
        while (*newdesc == ' ') newdesc++;

        // Search inventory then room
        uint16_t ids[8]; int found = -1;
        int n = world_.findItemsOnPlayer(s.nodeNum, ids, 8);
        for (int i = 0; i < n && found < 0; i++) {
            MudItem item;
            if (world_.loadItem(ids[i], item) && prefixMatchCI(item.name, prefix))
                found = (int)ids[i];
        }
        if (found < 0) {
            n = world_.findItemsInRoom(s.roomId, ids, 8);
            for (int i = 0; i < n && found < 0; i++) {
                MudItem item;
                if (world_.loadItem(ids[i], item) && prefixMatchCI(item.name, prefix))
                    found = (int)ids[i];
            }
        }
        if (found < 0) { snprintf(reply, len, "Item '%s' not found.", prefix); return false; }
        MudItem item;
        if (!world_.loadItem((uint16_t)found, item)) {
            snprintf(reply, len, "Item load error."); return false;
        }
        strncpy(item.desc, newdesc, sizeof(item.desc) - 1);
        item.desc[sizeof(item.desc) - 1] = '\0';
        world_.saveItem(item);
        snprintf(reply, len, "Updated desc of '%s'.", item.name);

    } else {
        snprintf(reply, len,
                 "Admin cmds:\n"
                 "@desc @name @dig\n"
                 "@link @create @idesc");
    }
    return false;
}


// ── Room description builder ───────────────────────────────────────────────

void MudGame::buildLook(const MudRoom &room, const MudSession &viewer,
                        char *buf, size_t len) {
    size_t written = (size_t)snprintf(buf, len, "[%s]\n%s\nExits:", room.name, room.desc);
    if (written >= len) return;

    if (room.numExits == 0) {
        strncat(buf, " none", len - strlen(buf) - 1);
    } else {
        for (int i = 0; i < room.numExits; i++) {
            if (i > 0) strncat(buf, ",", len - strlen(buf) - 1);
            strncat(buf, " ", len - strlen(buf) - 1);
            strncat(buf, room.exits[i].dir, len - strlen(buf) - 1);
        }
    }

    // Items in room
    uint16_t itemIds[6];
    int numItems = world_.findItemsInRoom(room.id, itemIds, 6);
    if (numItems > 0) {
        strncat(buf, "\nItems:", len - strlen(buf) - 1);
        for (int i = 0; i < numItems; i++) {
            MudItem item;
            if (!world_.loadItem(itemIds[i], item)) continue;
            strncat(buf, i > 0 ? "," : " ", len - strlen(buf) - 1);
            strncat(buf, item.name, len - strlen(buf) - 1);
        }
    }

    // Other players in room
    bool firstOther = true;
    for (int i = 0; i < MUD_MAX_SESSIONS; i++) {
        if (!sessions_[i].active) continue;
        if (sessions_[i].nodeNum == viewer.nodeNum) continue;
        if (sessions_[i].roomId != room.id) continue;
        if (firstOther) {
            strncat(buf, "\nAlso here:", len - strlen(buf) - 1);
            firstOther = false;
        }
        strncat(buf, " ", len - strlen(buf) - 1);
        strncat(buf, sessions_[i].name, len - strlen(buf) - 1);
    }
}

// ── Broadcast / DM helpers ─────────────────────────────────────────────────

void MudGame::broadcastRoom(uint16_t roomId, uint32_t excludeNode, const char *msg) {
    for (int i = 0; i < MUD_MAX_SESSIONS; i++) {
        if (!sessions_[i].active) continue;
        if (sessions_[i].nodeNum == excludeNode) continue;
        if (sessions_[i].roomId != roomId) continue;
        sendDM(sessions_[i].nodeNum, msg);
    }
}

void MudGame::sendDM(uint32_t toNode, const char *text) {
    if (!text || !router) return;
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) return;
    p->to                    = toNode;
    p->channel               = channels.getPrimaryIndex();
    p->want_ack              = false;
    p->decoded.portnum       = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.want_response = false;
    size_t n = strlen(text);
    if (n >= sizeof(p->decoded.payload.bytes))
        n = sizeof(p->decoded.payload.bytes) - 1;
    memcpy(p->decoded.payload.bytes, text, n);
    p->decoded.payload.size = n;
    service->sendToMesh(p);
}

// ── Direction utilities ────────────────────────────────────────────────────

const char *MudGame::oppositeDir(const char *dir) {
    if (!dir) return "";
    if (strcmp(dir, "n")  == 0) return "south";
    if (strcmp(dir, "s")  == 0) return "north";
    if (strcmp(dir, "e")  == 0) return "west";
    if (strcmp(dir, "w")  == 0) return "east";
    if (strcmp(dir, "u")  == 0) return "below";
    if (strcmp(dir, "d")  == 0) return "above";
    if (strcmp(dir, "ne") == 0) return "southwest";
    if (strcmp(dir, "nw") == 0) return "southeast";
    if (strcmp(dir, "se") == 0) return "northwest";
    if (strcmp(dir, "sw") == 0) return "northeast";
    return "";
}

const char *MudGame::reverseDir(const char *dir) {
    if (!dir) return "";
    if (strcmp(dir, "n")  == 0) return "s";
    if (strcmp(dir, "s")  == 0) return "n";
    if (strcmp(dir, "e")  == 0) return "w";
    if (strcmp(dir, "w")  == 0) return "e";
    if (strcmp(dir, "u")  == 0) return "d";
    if (strcmp(dir, "d")  == 0) return "u";
    if (strcmp(dir, "ne") == 0) return "sw";
    if (strcmp(dir, "nw") == 0) return "se";
    if (strcmp(dir, "se") == 0) return "nw";
    if (strcmp(dir, "sw") == 0) return "ne";
    return "";
}

// ── String utility ─────────────────────────────────────────────────────────

bool MudGame::prefixMatchCI(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix))
            return false;
        str++; prefix++;
    }
    return true;
}
