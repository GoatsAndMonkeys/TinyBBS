#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>

// ─── Directory / limits ────────────────────────────────────────────────────
#define FRPG_DIR          "/bbs/frpg"
#define FRPG_EASTERN_OFFSET (-14400)  // EDT = UTC-4; change to -18000 in winter (EST)
#define FRPG_AP_MAX       15
#define FRPG_AP_RESET_S   86400UL
#define FRPG_MAX_LEVEL    12
#define FRPG_STIMPAK_MAX  9
#define FRPG_STIMPAK_COST 50
#define FRPG_HACK_WORD_COUNT 200

// ─── Limb-effect bitfield (FRPGCombat.limbEffects) ────────────────────────
#define LIMB_STUNNED   0x01   // humanoid/creature: skip next attack (enemy)
#define LIMB_EXPOSE    0x02   // +25% damage from all hits (50% if crit VATS)
#define LIMB_DISARM    0x04   // enemy atk -40% (or -80% on crit VATS)
#define LIMB_WEAKEN    0x08   // humanoid: enemy atk -25% (stacks with DISARM)
#define LIMB_CRIPPLED  0x10   // creature/humanoid leg: player flee 90%; enemy dmg -30%
#define LIMB_BLIND     0x20   // robot head: enemy dmg -30%
#define LIMB_FRENZY    0x40   // robot inhibitor: enemy hits itself this round

// ─── Enemy type ────────────────────────────────────────────────────────────
#define ETYPE_HUMANOID  0
#define ETYPE_CREATURE  1
#define ETYPE_ROBOT     2

// ─── Data table structs ────────────────────────────────────────────────────
struct FRPGWeapon {
    const char *name;   // ≤16 chars
    uint8_t  damage;
    uint8_t  levelReq;
    uint16_t cost;
};

struct FRPGArmor {
    const char *name;   // ≤14 chars
    uint8_t  defense;
    uint8_t  levelReq;
    uint16_t cost;
};

struct FRPGEnemy {
    const char *name;
    uint8_t  tier;      // 1-4
    uint8_t  baseHp;    // hp = baseHp + level*5
    uint8_t  baseAtk;   // atk = baseAtk + level*2
    uint8_t  xpReward;
    uint8_t  capReward;
    uint8_t  etype;     // ETYPE_*
};

struct FRPGTrainer {
    const char *name;   // ≤16 chars
    uint16_t hp;        // base hp (some trainers exceed 255)
    uint8_t  atk;
    uint16_t xpReward;
    uint16_t capReward;
};

// ─── VATS target descriptor ────────────────────────────────────────────────
struct FRPGVATSPart {
    const char *label;   // display label e.g. "HEAD   [Stun]"
    uint8_t  hitMul10;  // hitChance = per * hitMul10 / 10, in percent
    uint8_t  hitCap;    // cap in percent
    uint8_t  effect;    // LIMB_* bit to apply on hit
};

// Body-part tables (4 or 5 entries each, terminated by hitMul10==0)
extern const FRPGVATSPart FRPG_VATS_HUMANOID[];  // 5 parts
extern const FRPGVATSPart FRPG_VATS_CREATURE[];  // 4 parts
extern const FRPGVATSPart FRPG_VATS_ROBOT[];     // 5 parts

// ─── Hack state ────────────────────────────────────────────────────────────
struct FRPGHack {
    uint8_t  active;        // 1 = terminal hack in progress
    uint8_t  targetIdx;     // which of words[12] is the correct answer
    uint8_t  attempts;      // remaining attempts
    uint8_t  difficulty;    // 0=Novice 1=Advanced 2=Expert 3=Master
    uint8_t  words[12];     // indices into FRPG_HACK_WORDS[]
    uint16_t visible;       // bitmask: bit N=1 means words[N] is still shown
    uint8_t  _pad;
};  // 18 bytes

// ─── Combat state ──────────────────────────────────────────────────────────
struct FRPGCombat {
    uint8_t  active;        // 0=none, 1=enemy, 2=trainer, 3=sec-bot (no flee)
    uint8_t  enemyIdx;      // index into FRPG_ENEMIES (active=1,3) or FRPG_TRAINERS (active=2)
    uint8_t  enemyType;     // ETYPE_*
    uint8_t  baseEnemyAtk;  // raw atk (before VATS mods) for reference
    uint8_t  limbEffects;   // bitfield LIMB_*
    uint8_t  stunRounds;    // rounds left on stun
    uint8_t  vatsModeActive;// 1 = waiting for VATS target number
    uint8_t  critExpose;    // 1 = expose is 50% (VATS crit), 0 = 25%
    uint8_t  defending;     // 1 if player chose Defend this round
    uint8_t  _pad;
    uint16_t enemyHp;
};  // 12 bytes

// ─── Player record ─────────────────────────────────────────────────────────
struct FRPGPlayer {
    // Identity
    uint32_t nodeNum;        // 4
    char     name[5];        // 5  (null-terminated short name ≤4 chars)
    uint8_t  _pad0;          // 1  alignment
    // Timestamps
    uint32_t apResetTime;    // 4
    uint32_t lastHealTime;   // 4
    uint32_t lastPvpTime;    // 4
    uint32_t lastPvpTarget;  // 4
    // Core stats
    uint32_t xp;             // 4
    uint16_t maxHp;          // 2
    uint16_t hp;             // 2
    uint16_t caps;           // 2
    uint8_t  level;          // 1
    uint8_t  str_;           // 1  STR  — note: 'str' conflicts with std string
    uint8_t  per;            // 1  PER
    uint8_t  end_;           // 1  END
    uint8_t  intel;          // 1  INT
    uint8_t  agi;            // 1  AGI
    // Equipment & consumables
    uint8_t  weapon;         // 1  index into FRPG_WEAPONS
    uint8_t  armor;          // 1  index into FRPG_ARMORS
    uint8_t  stimpaks;       // 1
    // Daily resources
    uint8_t  ap;             // 1  action points remaining
    uint8_t  vatsCharges;    // 1  VATS uses remaining today
    uint8_t  vatsMax;        // 1  max daily VATS = 2 + agi/4
    uint8_t  healedToday;    // 1  free clinic heal used
    uint8_t  alive;          // 1
    uint8_t  kills;          // 1  Lagoon Monster boss-ladder kills (unlocks Cheng)
    uint8_t  chengKills;     // 1  Chairman Cheng kills (prestige counter)
    // Transient state (persisted so crashes don't lose in-progress fights)
    FRPGCombat combat;       // 12
    FRPGHack   hack;         // 18
};  // ~92 bytes total

// ─── Table counts ───────────────────────────────────────────────────────────
static const uint8_t  FRPG_WEAPON_COUNT  = 12;
static const uint8_t  FRPG_ARMOR_COUNT   = 10;
static const uint8_t  FRPG_ENEMY_COUNT   = 16;
static const uint8_t  FRPG_TRAINER_COUNT = 12;

// ─── XP threshold to advance FROM level n (index 0 unused) ─────────────────
static const uint32_t FRPG_XP_THRESH[FRPG_MAX_LEVEL + 1] = {
    0, 100, 250, 500, 900, 1400, 2100, 3000, 4200, 5700, 7500, 9500, 12000
};

// ─── External tables ────────────────────────────────────────────────────────
extern const FRPGWeapon  FRPG_WEAPONS[12];
extern const FRPGArmor   FRPG_ARMORS[10];
extern const FRPGEnemy   FRPG_ENEMIES[16];
extern const FRPGTrainer FRPG_TRAINERS[12];
extern const char       *FRPG_HACK_WORDS[FRPG_HACK_WORD_COUNT];

// ─── Public announce flag (set by frpgVictory on Cheng kill) ────────────────
extern bool frpgPendingAnnounce;
extern char frpgAnnounceMsg[120];

// ─── Public API ─────────────────────────────────────────────────────────────
void     frpgEnsureDir();
bool     frpgLoadPlayer(uint32_t nodeNum, FRPGPlayer &p);
void     frpgSavePlayer(const FRPGPlayer &p);
void     frpgNewPlayer(uint32_t nodeNum, const char *shortName, FRPGPlayer &p);

// Main entry point.
//   nodeNum   — calling node's ID
//   text      — raw command text (already trimmed by BBS)
//   shortName — node's short name from nodeDB (may be NULL)
//   outBuf    — response buffer (caller should use sendReplyMultipart for >200b)
//   outLen    — size of outBuf (pass >=512 for ABOUT/SHOP list)
//   exitGame  — set to true when player uses [X]Back to BBS
void frpgCommand(uint32_t nodeNum, const char *text, const char *shortName,
                 char *outBuf, size_t outLen, bool &exitGame);

uint32_t frpgTopPlayers(FRPGPlayer *out, uint32_t max);
