#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>

// ─── Directory / limits ───────────────────────────────────────────────────────
#define WL_DIR          "/bbs/wl"
#define WL_AP_MAX       15
#define WL_AP_RESET_S   86400
#define WL_MAX_LEVEL    12
#define WL_STIMPAK_MAX  9
#define WL_STIMPAK_COST 50

// ─── Data tables ──────────────────────────────────────────────────────────────

struct WLWeapon {
    const char *name;   // short name ≤16 chars
    uint8_t  damage;
    uint8_t  levelReq;
    uint16_t cost;
};

struct WLArmor {
    const char *name;   // short name ≤12 chars
    uint8_t  defense;
    uint8_t  levelReq;
    uint16_t cost;
};

struct WLEnemy {
    const char *name;
    uint8_t  tier;      // 1-4
    uint8_t  baseHp;    // enemy hp = baseHp + playerLevel*5
    uint8_t  baseAtk;   // enemy atk = baseAtk + playerLevel*2
    uint8_t  xpReward;  // base XP
    uint8_t  capReward; // base caps
};

struct WLTrainer {
    const char *name;   // ≤14 chars
    uint8_t  hp;        // trainer hp (scaled in fight start)
    uint8_t  atk;       // trainer atk
    uint16_t xpReward;
    uint16_t capReward;
};

// ─── Player save record ───────────────────────────────────────────────────────

struct WLPlayer {
    uint32_t nodeNum;
    uint32_t apResetTime;    // epoch of last AP reset
    uint32_t lastHealTime;   // epoch of last free heal (daily)
    uint32_t lastPvpTime;    // epoch of last PvP attack
    uint32_t lastPvpTarget;  // nodeNum of last PvP target
    uint32_t xp;
    uint16_t maxHp;
    uint16_t hp;
    uint16_t caps;
    uint16_t enemyHp;        // current enemy HP in combat
    uint8_t  level;
    uint8_t  str;            // Strength
    uint8_t  per;            // Perception
    uint8_t  end;            // Endurance
    uint8_t  weapon;         // index into WL_WEAPONS
    uint8_t  armor;          // index into WL_ARMORS
    uint8_t  stimpaks;
    uint8_t  ap;             // remaining action points
    uint8_t  alive;          // 0=dead until next day reset
    uint8_t  kills;          // Deathclaw Alpha kills
    uint8_t  inCombat;       // 0=none 1=enemy 2=trainer/boss
    uint8_t  combatEnemy;    // enemy/trainer index
    uint8_t  enemyAtk;       // stored enemy atk for consistency
    uint8_t  defending;      // 1 if player used defend this round
    uint8_t  healedToday;    // 1 if free clinic heal used
    char     name[5];        // short name from node DB, null-terminated
};

// ─── Game table declarations (defined in .cpp) ────────────────────────────────

extern const WLWeapon  WL_WEAPONS[];
extern const WLArmor   WL_ARMORS[];
extern const WLEnemy   WL_ENEMIES[];
extern const WLTrainer WL_TRAINERS[];

static const uint8_t WL_WEAPON_COUNT  = 12;
static const uint8_t WL_ARMOR_COUNT   =  6;
static const uint8_t WL_ENEMY_COUNT   = 16;
static const uint8_t WL_TRAINER_COUNT = 12;

// XP needed to advance FROM level n (index 0 unused)
static const uint32_t WL_XP_THRESH[WL_MAX_LEVEL + 1] = {
    0, 100, 250, 500, 900, 1400, 2100, 3000, 4200, 5700, 7500, 9500, 12000
};

// ─── Public API ───────────────────────────────────────────────────────────────

void     wlEnsureDir();
bool     wlLoadPlayer(uint32_t nodeNum, WLPlayer &p);
void     wlSavePlayer(const WLPlayer &p);
void     wlNewPlayer(uint32_t nodeNum, const char *shortName, WLPlayer &p);
void     wlResetApIfNeeded(WLPlayer &p, uint32_t now);

// Main entry point — call from BBSModule with the player's nodeNum, their
// raw text, their short name from nodeDB, and an output buffer (≤200 bytes).
void     wlCommand(uint32_t nodeNum, const char *text, const char *shortName,
                   char *outBuf, size_t outLen);

// Leaderboard scan — fills out[] with up to max players, returns count.
uint32_t wlTopPlayers(WLPlayer *out, uint32_t max);
