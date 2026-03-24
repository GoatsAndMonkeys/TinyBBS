#pragma once
// MudGame.h — session management and command dispatch for TinyMUD

#include "MudWorld.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <cstdint>

#define MUD_MAX_SESSIONS     6
#define MUD_SESSION_TIMEOUT  1800U   // 30 min idle timeout
#define MUD_REPLY_MAX        200

struct MudSession {
    uint32_t nodeNum;
    uint16_t roomId;
    uint32_t lastActivity;  // unix timestamp
    char     name[16];      // short display name (from Meshtastic node)
    uint8_t  flags;
    bool     active;
};

class MudGame {
  public:
    MudGame();

    // Called when a player first enters the MUD from the BBS menu.
    // Populates reply with the welcome text + room look.
    void enter(uint32_t nodeNum, const char *shortName,
               char *reply, size_t replyLen);

    // Process one command from nodeNum.
    // Populates reply with the response text (may be empty if handled internally).
    // Returns true if the player quit the MUD (caller should return to BBS main menu).
    bool handle(const meshtastic_MeshPacket &mp, const char *text,
                char *reply, size_t replyLen);

  private:
    MudWorld    world_;
    MudSession  sessions_[MUD_MAX_SESSIONS];
    bool        worldReady_ = false;

    // ── Session helpers ────────────────────────────────────────────────────
    bool          ensureWorld();
    MudSession   *getSession(uint32_t nodeNum);
    MudSession   *createSession(uint32_t nodeNum, const char *name, uint16_t roomId);
    void          expireSessions(uint32_t now);
    void          persistSession(const MudSession &s);

    // ── Command handlers ───────────────────────────────────────────────────
    // Return true if player quit MUD.
    bool cmdLook(MudSession &s, char *reply, size_t len);
    bool cmdGo(MudSession &s, const char *dir, char *reply, size_t len);
    bool cmdSay(MudSession &s, const char *msg);
    bool cmdWho(char *reply, size_t len);
    bool cmdHelp(char *reply, size_t len);
    bool cmdQuit(MudSession &s, char *reply, size_t len);

    // ── Item commands ──────────────────────────────────────────────────────
    bool cmdInventory(MudSession &s, char *reply, size_t len);
    bool cmdTake(MudSession &s, const char *name, char *reply, size_t len);
    bool cmdDrop(MudSession &s, const char *name, char *reply, size_t len);
    bool cmdExamine(MudSession &s, const char *name, char *reply, size_t len);

    // ── Admin commands (@prefix) ───────────────────────────────────────────
    // subcmd = word after @, arg = rest of line
    bool cmdAdminCmd(MudSession &s, const char *subcmd, const char *arg,
                     char *reply, size_t len);

    // ── Broadcast helpers ──────────────────────────────────────────────────
    void broadcastRoom(uint16_t roomId, uint32_t excludeNode, const char *msg);
    void sendDM(uint32_t toNode, const char *text);

    // ── Room description builder ───────────────────────────────────────────
    void buildLook(const MudRoom &room, const MudSession &viewer,
                   char *buf, size_t len);

    // ── Direction utilities ────────────────────────────────────────────────
    static const char *oppositeDir(const char *dir);  // long form ("south")
    static const char *reverseDir(const char *dir);   // short form ("s")

    // ── String utility ────────────────────────────────────────────────────
    static bool prefixMatchCI(const char *str, const char *prefix);
};
