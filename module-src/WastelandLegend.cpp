// WastelandLegend.cpp — Fallout-themed door RPG for Meshtastic BBS
// Every player-visible snprintf output is kept to ≤190 bytes (leaving margin
// within the 200-byte Meshtastic message limit).
//
// FSCom rules:
//   - FSCom.open(path, FILE_O_READ/"r")  / FSCom.open(path, FILE_O_WRITE/"w")
//   - f.read() / f.write() / f.close()
//   - FSCom.exists() / FSCom.mkdir() / FSCom.remove()
//   - Dir iteration: dir.openNextFile() / f.isDirectory() / f.name()
//   - NEVER fopen/fclose/LittleFS.begin()

#include "FSCommon.h"
#include "RTC.h"
#include <Arduino.h>
#include "WastelandLegend.h"
#include <cctype>
#include <cstring>
#include <cstdio>

// ─── Game Tables ──────────────────────────────────────────────────────────────

const WLWeapon WL_WEAPONS[12] = {
    {"Pipe Pistol",    5,  1,     0},
    {"Baseball Bat",   8,  1,   150},
    {"10mm Pistol",   12,  2,   300},
    {"Hunting Rifle", 18,  3,   500},
    {"Cbt Shotgun",   25,  4,   800},
    {"Laser Pistol",  32,  5,  1200},
    {"Plasma Rifle",  40,  6,  1800},
    {"Minigun",       50,  7,  2500},
    {"Gauss Rifle",   62,  8,  3500},
    {"Msle Launcher", 75,  9,  5000},
    {"Fat Man",       90, 10,  7000},
    {"Alien Blaster",110, 11, 10000},
};

const WLArmor WL_ARMORS[6] = {
    {"Vault Suit",    0,  1,    0},
    {"Leather Armor", 5,  2,  200},
    {"Metal Armor",  10,  4,  600},
    {"Combat Armor", 18,  6, 1500},
    {"Synth Armor",  28,  9, 4000},
    {"Power Armor",  40, 11, 8000},
};

const WLEnemy WL_ENEMIES[16] = {
    // Tier 1 (levels 1-3)
    {"Radroach",       1, 15,  6, 10, 15},
    {"Bloatfly",       1, 12,  5, 12, 12},
    {"Mole Rat",       1, 20,  8, 15, 18},
    {"Feral Ghoul",    1, 18,  9, 18, 20},
    // Tier 2 (levels 4-6)
    {"Raider",         2, 35, 15, 30, 40},
    {"Yao Guai",       2, 40, 18, 35, 45},
    {"Radscorpion",    2, 38, 16, 33, 42},
    {"Super Mutant",   2, 45, 20, 40, 55},
    // Tier 3 (levels 7-9)
    {"Yng Deathclaw",  3, 70, 30, 70,100},
    {"Assaultron",     3, 65, 28, 65, 90},
    {"Sentry Bot",     3, 75, 32, 75,110},
    {"Mirelurk Qn",   3, 80, 35, 80,120},
    // Tier 4 (levels 10-12)
    {"Behemoth",       4,120, 50,130,200},
    {"Mythc Deathclaw",4,130, 55,140,220},
    {"Fog Crawler",    4,110, 48,125,190},
    {"Asltron Domtr",  4,125, 52,135,210},
};

const WLTrainer WL_TRAINERS[12] = {
    {"Scavenger",      40, 10,  150,   50},  // beats to go 1→2
    {"Raider Punk",    65, 14,  250,   80},  // 2→3
    {"Mole Wranglr",   90, 18,  400,  120},  // 3→4
    {"Caravan Guard", 120, 22,  600,  180},  // 4→5
    {"BOS Initiate",  155, 27,  900,  250},  // 5→6
    {"Ghoul Veteran", 185, 32, 1300,  350},  // 6→7
    {"Mutant Brute",  220, 38, 1800,  500},  // 7→8
    {"Enclave Sldier",260, 44, 2400,  700},  // 8→9
    {"Ranger Cmdr",   300, 50, 3200,  900},  // 9→10
    {"Paladin",       340, 56, 4200, 1200},  // 10→11
    {"Sentinel",      385, 63, 5500, 1600},  // 11→12
    {"Deathclaw Alpha",500,80, 8000, 3000},  // boss (index 11)
};

// ─── Forward declarations for internal helpers ────────────────────────────────

static void wlShowMenu(WLPlayer &p, char *buf, size_t len);
static void wlShowCombat(const WLPlayer &p, char *buf, size_t len);
static void wlDoExplore(WLPlayer &p, char *buf, size_t len, uint32_t now);
static void wlDoAttack(WLPlayer &p, char *buf, size_t len);
static void wlDoDefend(WLPlayer &p, char *buf, size_t len);
static void wlDoStimpak(WLPlayer &p, char *buf, size_t len, bool inCombat);
static void wlDoFlee(WLPlayer &p, char *buf, size_t len);
static void wlDoHeal(WLPlayer &p, char *buf, size_t len, uint32_t now);
static void wlDoStats(const WLPlayer &p, char *buf, size_t len);
static void wlDoShop(WLPlayer &p, const char *arg, char *buf, size_t len);
static void wlDoTrain(WLPlayer &p, char *buf, size_t len);
static void wlDoArena(WLPlayer &p, const char *arg, char *buf, size_t len,
                      uint32_t now, uint32_t selfNode);
static void wlDoLeaderboard(const WLPlayer &p, char *buf, size_t len);

// ─── Combat math helpers ──────────────────────────────────────────────────────

// Returns damage dealt by the player (0 = miss).
static int wlPlayerDmg(const WLPlayer &p)
{
    int hitChance = 75 + p.per * 2;
    if (hitChance > 95) hitChance = 95;
    if (random(100) >= hitChance) return 0; // miss
    int dmg = WL_WEAPONS[p.weapon].damage + p.str / 2
              + (int)random(p.str / 2 + 1);
    if (random(100) < (int)p.per) dmg *= 2; // critical
    return dmg;
}

// Returns damage taken by the player (after armor, defend halving).
static int wlEnemyDmg(const WLPlayer &p)
{
    int dmg = p.enemyAtk + (int)random(p.enemyAtk / 2 + 1);
    dmg -= WL_ARMORS[p.armor].defense;
    if (p.defending) dmg /= 2;
    if (dmg < 1) dmg = 1;
    return dmg;
}

static int wlStimpakHeal(const WLPlayer &p)
{
    return 30 + p.level * 3;
}

// ─── Tier / enemy selection ───────────────────────────────────────────────────

static uint8_t wlGetTier(uint8_t level)
{
    int t = (level - 1) / 3 + 1;
    if (t < 1) t = 1;
    if (t > 4) t = 4;
    return (uint8_t)t;
}

static void wlPickEnemy(uint8_t level, uint8_t &enemyIdx,
                        uint16_t &enemyHp, uint8_t &enemyAtk)
{
    uint8_t tier = wlGetTier(level);
    // Collect indices for this tier (always 4 per tier)
    uint8_t pool[4];
    uint8_t poolSz = 0;
    for (uint8_t i = 0; i < WL_ENEMY_COUNT && poolSz < 4; i++) {
        if (WL_ENEMIES[i].tier == tier) pool[poolSz++] = i;
    }
    if (poolSz == 0) { enemyIdx = 0; } // fallback
    else              { enemyIdx = pool[random(poolSz)]; }

    enemyHp  = (uint16_t)(WL_ENEMIES[enemyIdx].baseHp + level * 5);
    enemyAtk = (uint8_t)(WL_ENEMIES[enemyIdx].baseAtk + level * 2);
}

// ─── File I/O ─────────────────────────────────────────────────────────────────

void wlEnsureDir()
{
    if (!FSCom.exists("/bbs"))   FSCom.mkdir("/bbs");
    if (!FSCom.exists(WL_DIR))   FSCom.mkdir(WL_DIR);
}

static void wlPlayerPath(uint32_t nodeNum, char *path, size_t len)
{
    snprintf(path, len, "%s/p%08x.bin", WL_DIR, (unsigned)nodeNum);
}

bool wlLoadPlayer(uint32_t nodeNum, WLPlayer &p)
{
    char path[40];
    wlPlayerPath(nodeNum, path, sizeof(path));
    File f = FSCom.open(path, FILE_O_READ);
    if (!f) return false;
    size_t n = f.read((uint8_t *)&p, sizeof(WLPlayer));
    f.close();
    return (n == sizeof(WLPlayer));
}

void wlSavePlayer(const WLPlayer &p)
{
    char path[40];
    wlPlayerPath(p.nodeNum, path, sizeof(path));
    File f = FSCom.open(path, FILE_O_WRITE);
    if (!f) return;
    f.write((const uint8_t *)&p, sizeof(WLPlayer));
    f.close();
}

void wlNewPlayer(uint32_t nodeNum, const char *shortName, WLPlayer &p)
{
    memset(&p, 0, sizeof(WLPlayer));
    p.nodeNum      = nodeNum;
    p.level        = 1;
    p.xp           = 0;
    p.maxHp        = 50;
    p.hp           = 50;
    p.str          = 5;
    p.per          = 5;
    p.end          = 5;
    p.caps         = 100;
    p.stimpaks     = 2;
    p.ap           = WL_AP_MAX;
    p.weapon       = 0;
    p.armor        = 0;
    p.alive        = 1;
    p.kills        = 0;
    p.inCombat     = 0;
    p.defending    = 0;
    p.healedToday  = 0;
    p.apResetTime  = (uint32_t)getTime();
    p.lastHealTime = 0;
    p.lastPvpTime  = 0;
    p.lastPvpTarget= 0;
    strncpy(p.name, shortName ? shortName : "Wandr", 4);
    p.name[4] = '\0';
}

void wlResetApIfNeeded(WLPlayer &p, uint32_t now)
{
    if (now >= p.apResetTime &&
        (now - p.apResetTime) >= (uint32_t)WL_AP_RESET_S) {
        p.ap            = WL_AP_MAX;
        p.alive         = 1;
        p.healedToday   = 0;
        p.lastPvpTarget = 0;
        p.apResetTime   = now;
    }
}

// ─── Leaderboard scan ─────────────────────────────────────────────────────────

uint32_t wlTopPlayers(WLPlayer *out, uint32_t max)
{
    uint32_t count = 0;
    File dir = FSCom.open(WL_DIR, FILE_O_READ);
    if (!dir) return 0;

    File f;
    while ((f = dir.openNextFile()) && count < max) {
        if (f.isDirectory()) { f.close(); continue; }
        // Only process files matching p*.bin pattern
        const char *nm = f.name();
        if (nm[0] != 'p') { f.close(); continue; }
        WLPlayer tmp;
        size_t n = f.read((uint8_t *)&tmp, sizeof(WLPlayer));
        f.close();
        if (n != sizeof(WLPlayer)) continue;

        // Insertion-sort by kills DESC then level DESC
        out[count] = tmp;
        for (uint32_t i = count; i > 0; i--) {
            WLPlayer &a = out[i];
            WLPlayer &b = out[i - 1];
            bool swap = (a.kills > b.kills) ||
                        (a.kills == b.kills && a.level > b.level);
            if (!swap) break;
            WLPlayer t = a; a = b; b = t;
        }
        count++;
    }
    dir.close();
    return count;
}

// ─── Menu display ─────────────────────────────────────────────────────────────

static void wlShowMenu(WLPlayer &p, char *buf, size_t len)
{
    bool trainReady = (p.level < WL_MAX_LEVEL) &&
                      (p.xp >= WL_XP_THRESH[p.level]);
    bool bossReady  = (p.level == WL_MAX_LEVEL) &&
                      (p.xp >= WL_XP_THRESH[WL_MAX_LEVEL]);

    if (!p.alive) {
        // Calculate time until next reset
        uint32_t now = (uint32_t)getTime();
        uint32_t elapsed = (now >= p.apResetTime) ? (now - p.apResetTime) : 0;
        uint32_t remain  = (elapsed < (uint32_t)WL_AP_RESET_S)
                           ? (WL_AP_RESET_S - elapsed) : 0;
        uint32_t h = remain / 3600;
        uint32_t m = (remain % 3600) / 60;
        snprintf(buf, len,
            "=== Wasteland Legend ===\n"
            "You died! Respawn in %uh%um\n"
            "[H]eal [SH]op\n"
            "[ST]ats [LB]oard\n"
            "[AR]ena [X]Back",
            (unsigned)h, (unsigned)m);
        return;
    }

    if (trainReady || bossReady) {
        snprintf(buf, len,
            "=== Wasteland Legend ===\n"
            "[E]xplore AP:%d/%d\n"
            "[H]eal [SH]op\n"
            "[ST]ats [LB]oard\n"
            "[TR]ain (READY!) [AR]ena\n"
            "[X]Back to BBS",
            p.ap, WL_AP_MAX);
    } else {
        snprintf(buf, len,
            "=== Wasteland Legend ===\n"
            "[E]xplore AP:%d/%d\n"
            "[H]eal [SH]op\n"
            "[ST]ats [LB]oard\n"
            "[TR]ain [AR]ena\n"
            "[X]Back to BBS",
            p.ap, WL_AP_MAX);
    }
}

// ─── Combat status display ────────────────────────────────────────────────────

static void wlShowCombat(const WLPlayer &p, char *buf, size_t len)
{
    const char *eName = (p.inCombat == 2)
        ? WL_TRAINERS[p.combatEnemy].name
        : WL_ENEMIES[p.combatEnemy].name;
    snprintf(buf, len,
        "Still fighting %s!\n"
        "EHP:%d ATK:%d\n"
        "Your HP:%d/%d\n"
        "A/D/S/F",
        eName, p.enemyHp, p.enemyAtk, p.hp, p.maxHp);
}

// ─── Explore ──────────────────────────────────────────────────────────────────

static void wlDoExplore(WLPlayer &p, char *buf, size_t len, uint32_t /*now*/)
{
    if (!p.alive) {
        snprintf(buf, len, "You are dead.\nRest until tomorrow to respawn.");
        return;
    }
    if (p.ap == 0) {
        snprintf(buf, len, "No AP left.\nRest until tomorrow.");
        return;
    }
    p.ap--;

    // 1-in-6 chance of random event instead of combat
    if (random(6) == 0) {
        switch (random(4)) {
            case 0:
                if (p.stimpaks < WL_STIMPAK_MAX) {
                    p.stimpaks++;
                    snprintf(buf, len,
                        "Lucky find! +1 Stimpak\nStimpaks:%d\nAP:%d/%d",
                        p.stimpaks, p.ap, WL_AP_MAX);
                } else {
                    p.caps = (uint16_t)(p.caps + 25);
                    snprintf(buf, len,
                        "Found Stimpak (full).\nSold for 25c!\nCaps:%d AP:%d/%d",
                        p.caps, p.ap, WL_AP_MAX);
                }
                break;
            case 1: {
                int found = 20 + p.level * 10;
                p.caps = (uint16_t)(p.caps + found);
                snprintf(buf, len,
                    "Scavenged a cache!\n+%d caps\nCaps:%d AP:%d/%d",
                    found, p.caps, p.ap, WL_AP_MAX);
                break;
            }
            case 2: {
                int dmg = 5 + p.level;
                p.hp = (p.hp > dmg) ? (uint16_t)(p.hp - dmg) : 1;
                snprintf(buf, len,
                    "Radiation storm!\n-%d HP\nHP:%d/%d AP:%d/%d",
                    dmg, p.hp, p.maxHp, p.ap, WL_AP_MAX);
                break;
            }
            default:
                snprintf(buf, len,
                    "A merchant waves hello\nbut moves on quickly.\nAP:%d/%d",
                    p.ap, WL_AP_MAX);
                break;
        }
        return;
    }

    // Enemy encounter
    uint8_t  eIdx;
    uint16_t eHp;
    uint8_t  eAtk;
    wlPickEnemy(p.level, eIdx, eHp, eAtk);

    p.inCombat   = 1;
    p.combatEnemy = eIdx;
    p.enemyHp    = eHp;
    p.enemyAtk   = eAtk;
    p.defending  = 0;

    snprintf(buf, len,
        "A %s appears!\nEHP:%d ATK:%d\nYour HP:%d/%d\nA/D/S/F",
        WL_ENEMIES[eIdx].name, eHp, eAtk, p.hp, p.maxHp);
}

// ─── Attack ───────────────────────────────────────────────────────────────────

static void wlDoAttack(WLPlayer &p, char *buf, size_t len)
{
    bool isBoss    = (p.inCombat == 2 && p.combatEnemy == WL_MAX_LEVEL - 1);
    bool isTrainer = (p.inCombat == 2);

    // Detect critical separately so we can label it
    int hitChance = 75 + p.per * 2;
    if (hitChance > 95) hitChance = 95;
    bool miss = (random(100) >= hitChance);
    bool crit = false;
    int pdmg = 0;
    if (!miss) {
        pdmg = WL_WEAPONS[p.weapon].damage + p.str / 2
               + (int)random(p.str / 2 + 1);
        crit = (random(100) < (int)p.per);
        if (crit) pdmg *= 2;
    }

    if (miss) {
        // Player missed — enemy still attacks
        int edamage = wlEnemyDmg(p);
        p.defending = 0;
        if (p.hp > (uint16_t)edamage) {
            p.hp = (uint16_t)(p.hp - edamage);
        } else {
            p.hp    = 0;
            p.alive = 0;
            uint16_t loss = (uint16_t)(p.caps / 5);
            p.caps = (p.caps > loss) ? (uint16_t)(p.caps - loss) : 0;
            p.inCombat  = 0;
            p.defending = 0;
            snprintf(buf, len,
                "Miss! %s hits back for %d.\nYou DIED! Lost %dc.\nRespawn tomorrow.",
                isTrainer ? WL_TRAINERS[p.combatEnemy].name
                          : WL_ENEMIES[p.combatEnemy].name,
                edamage, loss);
            return;
        }
        const char *eName = isTrainer ? WL_TRAINERS[p.combatEnemy].name
                                       : WL_ENEMIES[p.combatEnemy].name;
        snprintf(buf, len,
            "Miss! %s hits for %d.\nEHP:%d | Your HP:%d/%d\nA/D/S/F",
            eName, edamage, p.enemyHp, p.hp, p.maxHp);
        return;
    }

    // Hit — apply damage to enemy
    const char *critLabel = crit ? " CRIT!" : "";
    uint8_t savedCombatEnemy = p.combatEnemy;
    bool    savedIsTrainer   = isTrainer;
    bool    savedIsBoss      = isBoss;

    if ((uint16_t)pdmg >= p.enemyHp) {
        // Enemy defeated
        p.enemyHp = 0;
        p.inCombat  = 0;
        p.defending = 0;

        if (savedIsTrainer) {
            uint16_t xpGain  = WL_TRAINERS[savedCombatEnemy].xpReward;
            uint16_t capGain = WL_TRAINERS[savedCombatEnemy].capReward;
            p.xp   += xpGain;
            p.caps  = (uint16_t)(p.caps + capGain);

            if (savedIsBoss) {
                // Boss kill — stay at level 12
                p.kills++;
                snprintf(buf, len,
                    "DEATHCLAW ALPHA SLAIN!%s\n"
                    "Kills:%d You are legend!\n"
                    "+%dc +%uxp",
                    critLabel, p.kills, capGain, (unsigned)xpGain);
            } else {
                // Trainer kill — level up
                p.level++;
                p.maxHp = (uint16_t)(p.maxHp + 10 + p.end);
                p.hp    = p.maxHp;
                p.str   = (uint8_t)(p.str + 2);
                p.per   = (uint8_t)(p.per + 2);
                p.end   = (uint8_t)(p.end + 1);
                snprintf(buf, len,
                    "%s defeated!%s\nLEVEL UP! Lvl:%d\n"
                    "HP:%d STR:%d PER:%d END:%d\n"
                    "+%dc +%uxp",
                    WL_TRAINERS[savedCombatEnemy].name, critLabel,
                    p.level, p.maxHp, p.str, p.per, p.end,
                    capGain, (unsigned)xpGain);
            }
        } else {
            // Regular enemy kill
            uint16_t xpGain  = WL_ENEMIES[savedCombatEnemy].xpReward;
            uint16_t capGain = WL_ENEMIES[savedCombatEnemy].capReward;
            p.xp   += xpGain;
            p.caps  = (uint16_t)(p.caps + capGain);
            snprintf(buf, len,
                "%s defeated!%s\n+%uxp +%dc\nHP:%d/%d AP:%d/%d",
                WL_ENEMIES[savedCombatEnemy].name, critLabel,
                (unsigned)xpGain, capGain,
                p.hp, p.maxHp, p.ap, WL_AP_MAX);
        }
        return;
    }

    // Enemy survives — attack back
    p.enemyHp = (uint16_t)(p.enemyHp - pdmg);
    int edamage = wlEnemyDmg(p);
    p.defending = 0;

    const char *eName = savedIsTrainer ? WL_TRAINERS[savedCombatEnemy].name
                                        : WL_ENEMIES[savedCombatEnemy].name;

    if (p.hp > (uint16_t)edamage) {
        p.hp = (uint16_t)(p.hp - edamage);
        snprintf(buf, len,
            "Hit %s for %d%s!\nEnemy took %d, Hits for %d.\nEHP:%d | Your HP:%d/%d\nA/D/S/F",
            eName, pdmg, critLabel, pdmg, edamage,
            p.enemyHp, p.hp, p.maxHp);
    } else {
        // Player killed by counter-attack
        p.hp    = 0;
        p.alive = 0;
        uint16_t loss = (uint16_t)(p.caps / 5);
        p.caps = (p.caps > loss) ? (uint16_t)(p.caps - loss) : 0;
        p.inCombat  = 0;
        p.defending = 0;
        snprintf(buf, len,
            "Hit %s for %d%s.\n%s hits back for %d.\nYou DIED! Lost %dc.\nRespawn tomorrow.",
            eName, pdmg, critLabel, eName, edamage, loss);
    }
}

// ─── Defend ───────────────────────────────────────────────────────────────────

static void wlDoDefend(WLPlayer &p, char *buf, size_t len)
{
    p.defending = 1;
    int edamage = wlEnemyDmg(p); // already halved because defending=1
    p.defending = 1;              // keep set for next turn if needed

    const char *eName = (p.inCombat == 2)
        ? WL_TRAINERS[p.combatEnemy].name
        : WL_ENEMIES[p.combatEnemy].name;

    if (p.hp > (uint16_t)edamage) {
        p.hp = (uint16_t)(p.hp - edamage);
        snprintf(buf, len,
            "You brace! %s hits for %d.\nEHP:%d | Your HP:%d/%d\nA/D/S/F",
            eName, edamage, p.enemyHp, p.hp, p.maxHp);
    } else {
        p.hp    = 0;
        p.alive = 0;
        uint16_t loss = (uint16_t)(p.caps / 5);
        p.caps = (p.caps > loss) ? (uint16_t)(p.caps - loss) : 0;
        p.inCombat  = 0;
        p.defending = 0;
        snprintf(buf, len,
            "Braced but %s hits for %d.\nYou DIED! Lost %dc.\nRespawn tomorrow.",
            eName, edamage, loss);
    }
}

// ─── Stimpak ──────────────────────────────────────────────────────────────────

static void wlDoStimpak(WLPlayer &p, char *buf, size_t len, bool inCombat)
{
    if (p.stimpaks == 0) {
        snprintf(buf, len, "No Stimpaks! Buy more at the shop (SH).");
        return;
    }
    p.stimpaks--;
    int heal = wlStimpakHeal(p);
    uint16_t newHp = (uint16_t)(p.hp + heal);
    if (newHp > p.maxHp) newHp = p.maxHp;
    p.hp = newHp;

    if (inCombat) {
        // Enemy counter-attacks
        int edamage = wlEnemyDmg(p);
        const char *eName = (p.inCombat == 2)
            ? WL_TRAINERS[p.combatEnemy].name
            : WL_ENEMIES[p.combatEnemy].name;

        if (p.hp > (uint16_t)edamage) {
            p.hp = (uint16_t)(p.hp - edamage);
            snprintf(buf, len,
                "Stimpak +%d HP!\n%s hits for %d.\nEHP:%d | Your HP:%d/%d\nA/D/S/F",
                heal, eName, edamage, p.enemyHp, p.hp, p.maxHp);
        } else {
            p.hp    = 0;
            p.alive = 0;
            uint16_t loss = (uint16_t)(p.caps / 5);
            p.caps = (p.caps > loss) ? (uint16_t)(p.caps - loss) : 0;
            p.inCombat  = 0;
            p.defending = 0;
            snprintf(buf, len,
                "Stimpak! But %s hits for %d.\nYou DIED! Lost %dc.\nRespawn tomorrow.",
                eName, edamage, loss);
        }
    } else {
        snprintf(buf, len,
            "Stimpak! HP:%d/%d\nStimpaks:%d",
            p.hp, p.maxHp, p.stimpaks);
    }
}

// ─── Flee ─────────────────────────────────────────────────────────────────────

static void wlDoFlee(WLPlayer &p, char *buf, size_t len)
{
    int fleeChance = 50 + p.per * 3;
    if (fleeChance > 85) fleeChance = 85;

    if (random(100) < fleeChance) {
        p.inCombat  = 0;
        p.defending = 0;
        snprintf(buf, len, "Escaped! AP:%d/%d\nBack to settlement.", p.ap, WL_AP_MAX);
        return;
    }

    // Failed — enemy attacks
    int edamage = wlEnemyDmg(p);
    p.defending = 0;
    const char *eName = (p.inCombat == 2)
        ? WL_TRAINERS[p.combatEnemy].name
        : WL_ENEMIES[p.combatEnemy].name;

    if (p.hp > (uint16_t)edamage) {
        p.hp = (uint16_t)(p.hp - edamage);
        snprintf(buf, len,
            "Trapped! %s hits for %d.\nYour HP:%d/%d | A/D/S/F",
            eName, edamage, p.hp, p.maxHp);
    } else {
        p.hp    = 0;
        p.alive = 0;
        uint16_t loss = (uint16_t)(p.caps / 5);
        p.caps = (p.caps > loss) ? (uint16_t)(p.caps - loss) : 0;
        p.inCombat  = 0;
        p.defending = 0;
        snprintf(buf, len,
            "Trapped! %s kills you for %d.\nLost %dc. Respawn tomorrow.",
            eName, edamage, loss);
    }
}

// ─── Free clinic heal ─────────────────────────────────────────────────────────

static void wlDoHeal(WLPlayer &p, char *buf, size_t len, uint32_t now)
{
    if (p.healedToday) {
        snprintf(buf, len, "Already healed today.\nCome back tomorrow.");
        return;
    }
    p.hp          = p.maxHp;
    p.healedToday = 1;
    p.lastHealTime = now;
    snprintf(buf, len,
        "Doc heals you!\nHP:%d/%d fully restored.\nStay safe out there.",
        p.hp, p.maxHp);
}

// ─── Stats ────────────────────────────────────────────────────────────────────

static void wlDoStats(const WLPlayer &p, char *buf, size_t len)
{
    uint32_t xpNeeded = (p.level <= WL_MAX_LEVEL) ? WL_XP_THRESH[p.level] : 0;
    snprintf(buf, len,
        "[%s] Lvl:%d XP:%u/%u\n"
        "HP:%d/%d AP:%d/%d\n"
        "STR:%d PER:%d END:%d\n"
        "Wpn:%s\n"
        "Arm:%s\n"
        "Caps:%d Stimps:%d\n"
        "Kills:%d",
        p.name, p.level, (unsigned)p.xp, (unsigned)xpNeeded,
        p.hp, p.maxHp, p.ap, WL_AP_MAX,
        p.str, p.per, p.end,
        WL_WEAPONS[p.weapon].name,
        WL_ARMORS[p.armor].name,
        p.caps, p.stimpaks,
        p.kills);
}

// ─── Shop ─────────────────────────────────────────────────────────────────────

// Shop item descriptor used for building the numbered list
struct ShopItem {
    uint8_t type;   // 0 = weapon, 1 = armor, 2 = stimpak
    uint8_t idx;    // index into WL_WEAPONS / WL_ARMORS (unused for stimpak)
};

static void wlDoShop(WLPlayer &p, const char *arg, char *buf, size_t len)
{
    // Build shop list:
    //   - Up to 2 weapons: levelReq <= p.level+2, not currently equipped, cheapest first
    //   - Up to 2 armors: same criteria
    //   - Always: stimpak
    ShopItem items[5];
    uint8_t  itemCount = 0;

    // Weapons — sorted by cost (already sorted in table)
    uint8_t wpnAdded = 0;
    for (uint8_t i = 0; i < WL_WEAPON_COUNT && wpnAdded < 2; i++) {
        if (i == p.weapon) continue;
        if (WL_WEAPONS[i].levelReq > p.level + 2) continue;
        items[itemCount++] = {0, i};
        wpnAdded++;
    }
    // Armors — sorted by cost (already sorted in table)
    uint8_t armAdded = 0;
    for (uint8_t i = 0; i < WL_ARMOR_COUNT && armAdded < 2; i++) {
        if (i == p.armor) continue;
        if (WL_ARMORS[i].levelReq > p.level + 2) continue;
        items[itemCount++] = {1, i};
        armAdded++;
    }
    // Stimpak always last
    items[itemCount++] = {2, 0};

    // ── Buying? ──
    // arg may be: "1", "2", "s", "b1", "b 1", "b2", "bs", " 1" etc.
    const char *a = arg;
    while (*a == ' ') a++;
    if (*a == 'b' || *a == 'B') { a++; while (*a == ' ') a++; }

    if (*a != '\0') {
        // Parse buy
        int buyIdx = -1;
        if (*a == 's' || *a == 'S') {
            // stimpak
            buyIdx = (int)itemCount - 1; // always last
        } else if (*a >= '1' && *a <= '9') {
            buyIdx = (*a - '1'); // 0-based
        }

        if (buyIdx < 0 || buyIdx >= (int)itemCount) {
            snprintf(buf, len, "Invalid item. Type SH to see shop.");
            return;
        }
        ShopItem &it = items[buyIdx];

        if (it.type == 0) {
            // Weapon
            const WLWeapon &w = WL_WEAPONS[it.idx];
            if (p.level < w.levelReq) {
                snprintf(buf, len, "Need Lvl %d for %s.", w.levelReq, w.name);
                return;
            }
            if (p.caps < w.cost) {
                snprintf(buf, len, "Need %dc for %s.\nYou have %dc.", w.cost, w.name, p.caps);
                return;
            }
            p.caps   = (uint16_t)(p.caps - w.cost);
            p.weapon = it.idx;
            snprintf(buf, len, "Equipped %s!\nCaps:%d", w.name, p.caps);
        } else if (it.type == 1) {
            // Armor
            const WLArmor &arm = WL_ARMORS[it.idx];
            if (p.level < arm.levelReq) {
                snprintf(buf, len, "Need Lvl %d for %s.", arm.levelReq, arm.name);
                return;
            }
            if (p.caps < arm.cost) {
                snprintf(buf, len, "Need %dc for %s.\nYou have %dc.", arm.cost, arm.name, p.caps);
                return;
            }
            p.caps  = (uint16_t)(p.caps - arm.cost);
            p.armor = it.idx;
            snprintf(buf, len, "Equipped %s!\nCaps:%d", arm.name, p.caps);
        } else {
            // Stimpak
            if (p.stimpaks >= WL_STIMPAK_MAX) {
                snprintf(buf, len, "Stimpak bag full! (max %d)", WL_STIMPAK_MAX);
                return;
            }
            if (p.caps < WL_STIMPAK_COST) {
                snprintf(buf, len, "Need %dc for Stimpak.\nYou have %dc.",
                         WL_STIMPAK_COST, p.caps);
                return;
            }
            p.caps = (uint16_t)(p.caps - WL_STIMPAK_COST);
            p.stimpaks++;
            snprintf(buf, len, "Stimpak bought!\nStimpaks:%d Caps:%d", p.stimpaks, p.caps);
        }
        return;
    }

    // ── Show shop listing ──
    // Build compact listing
    char listing[190] = "=== Trading Post ===\n";
    char line[50];
    for (uint8_t i = 0; i < itemCount; i++) {
        ShopItem &it = items[i];
        if (it.type == 0) {
            snprintf(line, sizeof(line), "%d. %s %dc Lv%d\n",
                i + 1,
                WL_WEAPONS[it.idx].name,
                WL_WEAPONS[it.idx].cost,
                WL_WEAPONS[it.idx].levelReq);
        } else if (it.type == 1) {
            snprintf(line, sizeof(line), "%d. %s %dc Lv%d\n",
                i + 1,
                WL_ARMORS[it.idx].name,
                WL_ARMORS[it.idx].cost,
                WL_ARMORS[it.idx].levelReq);
        } else {
            snprintf(line, sizeof(line), "S. Stimpak %dc (have:%d)\n",
                WL_STIMPAK_COST, p.stimpaks);
        }
        strncat(listing, line, sizeof(listing) - strlen(listing) - 1);
    }
    strncat(listing, "Buy: SH 1, SH 2...", sizeof(listing) - strlen(listing) - 1);
    snprintf(buf, len, "%s", listing);
}

// ─── Train ────────────────────────────────────────────────────────────────────

static void wlDoTrain(WLPlayer &p, char *buf, size_t len)
{
    if (p.level < WL_MAX_LEVEL) {
        // Normal trainer progression
        if (p.xp < WL_XP_THRESH[p.level]) {
            uint32_t needed = WL_XP_THRESH[p.level] - p.xp;
            snprintf(buf, len,
                "Not ready to train.\nNeed %u more XP.\nXP:%u/%u",
                (unsigned)needed, (unsigned)p.xp,
                (unsigned)WL_XP_THRESH[p.level]);
            return;
        }
        // Start trainer fight (index = level-1)
        uint8_t tidx = (uint8_t)(p.level - 1);
        p.inCombat    = 2;
        p.combatEnemy = tidx;
        p.enemyHp     = WL_TRAINERS[tidx].hp;
        p.enemyAtk    = WL_TRAINERS[tidx].atk;
        p.defending   = 0;
        snprintf(buf, len,
            "You challenge %s!\nEHP:%d ATK:%d\nA/D/S/F",
            WL_TRAINERS[tidx].name, p.enemyHp, p.enemyAtk);
    } else {
        // Level 12 — boss fight (Deathclaw Alpha, index 11)
        if (p.xp < WL_XP_THRESH[WL_MAX_LEVEL]) {
            uint32_t needed = WL_XP_THRESH[WL_MAX_LEVEL] - p.xp;
            snprintf(buf, len,
                "Not ready for boss.\nNeed %u more XP.\nXP:%u/%u",
                (unsigned)needed, (unsigned)p.xp,
                (unsigned)WL_XP_THRESH[WL_MAX_LEVEL]);
            return;
        }
        uint8_t tidx = WL_MAX_LEVEL - 1; // index 11 = Deathclaw Alpha
        p.inCombat    = 2;
        p.combatEnemy = tidx;
        p.enemyHp     = WL_TRAINERS[tidx].hp;
        p.enemyAtk    = WL_TRAINERS[tidx].atk;
        p.defending   = 0;
        snprintf(buf, len,
            "DEATHCLAW ALPHA emerges!\nEHP:%d ATK:%d\nFight for legend!\nA/D/S/F",
            p.enemyHp, p.enemyAtk);
    }
}

// ─── Arena (PvP) ──────────────────────────────────────────────────────────────

static void wlDoArena(WLPlayer &p, const char *arg, char *buf, size_t len,
                      uint32_t now, uint32_t selfNode)
{
    if (p.ap < 3) {
        snprintf(buf, len, "Need 3 AP for Arena.\nAP:%d/%d", p.ap, WL_AP_MAX);
        return;
    }

    // Skip leading spaces
    while (*arg == ' ') arg++;
    if (!*arg) {
        snprintf(buf, len,
            "=== Arena ===\n"
            "Challenge a player!\n"
            "AR <name>  (costs 3 AP)\n"
            "AR <partial name>");
        return;
    }

    // Find target by scanning player files
    WLPlayer target;
    bool found = false;
    char targetPath[40] = {0};

    File dir = FSCom.open(WL_DIR, FILE_O_READ);
    if (dir) {
        File f;
        while ((f = dir.openNextFile())) {
            if (f.isDirectory()) { f.close(); continue; }
            const char *nm = f.name();
            if (nm[0] != 'p') { f.close(); continue; }
            WLPlayer tmp;
            size_t n = f.read((uint8_t *)&tmp, sizeof(WLPlayer));
            f.close();
            if (n != sizeof(WLPlayer)) continue;
            if (tmp.nodeNum == selfNode) continue; // skip self

            // Case-insensitive partial match on name
            char tmpLow[5], argLow[5];
            strncpy(tmpLow, tmp.name, 4); tmpLow[4] = '\0';
            strncpy(argLow, arg, 4);      argLow[4] = '\0';
            for (int i = 0; tmpLow[i]; i++) tmpLow[i] = (char)tolower((unsigned char)tmpLow[i]);
            for (int i = 0; argLow[i]; i++) argLow[i] = (char)tolower((unsigned char)argLow[i]);

            if (strstr(tmpLow, argLow)) {
                target = tmp;
                snprintf(targetPath, sizeof(targetPath), "%s/p%08x.bin",
                         WL_DIR, (unsigned)tmp.nodeNum);
                found = true;
                break;
            }
        }
        dir.close();
    }

    if (!found) {
        snprintf(buf, len, "Player '%s' not found.\nCheck name and try again.", arg);
        return;
    }
    if (p.lastPvpTarget == target.nodeNum &&
        (now - p.lastPvpTime) < (uint32_t)WL_AP_RESET_S) {
        snprintf(buf, len, "Already fought %s today.\nTry again tomorrow.", target.name);
        return;
    }

    p.ap -= 3;
    p.lastPvpTarget = target.nodeNum;
    p.lastPvpTime   = now;

    // ── Simulate full PvP combat (auto-resolve, not turn-based) ──
    int aHp = (int)p.hp;
    int bHp = (int)target.hp;
    bool playerWon = false;

    for (int round = 0; round < 20 && aHp > 0 && bHp > 0; round++) {
        // Player attacks target
        {
            int hc = 75 + p.per * 2;
            if (hc > 95) hc = 95;
            if (random(100) < hc) {
                int dmg = WL_WEAPONS[p.weapon].damage + p.str / 2
                          + (int)random(p.str / 2 + 1);
                if (random(100) < (int)p.per) dmg *= 2;
                dmg -= WL_ARMORS[target.armor].defense;
                if (dmg < 1) dmg = 1;
                bHp -= dmg;
            }
        }
        if (bHp <= 0) { playerWon = true; break; }

        // Target attacks player
        {
            int hc = 75 + target.per * 2;
            if (hc > 95) hc = 95;
            if (random(100) < hc) {
                int dmg = WL_WEAPONS[target.weapon].damage + target.str / 2
                          + (int)random(target.str / 2 + 1);
                if (random(100) < (int)target.per) dmg *= 2;
                dmg -= WL_ARMORS[p.armor].defense;
                if (dmg < 1) dmg = 1;
                aHp -= dmg;
            }
        }
    }
    if (aHp > 0 && bHp > 0) playerWon = true; // survived longer = win

    int xpGain  = 30 + p.level * 5;
    int capGain = 50 + p.level * 10;

    if (playerWon) {
        p.xp   += (uint32_t)xpGain;
        p.caps  = (uint16_t)(p.caps + capGain);
        // Target loses 10% caps and is set dead
        uint16_t loss = (uint16_t)(target.caps / 10);
        target.caps = (target.caps > loss) ? (uint16_t)(target.caps - loss) : 0;
        target.alive = 0;
        wlSavePlayer(target);
        snprintf(buf, len,
            "You beat %s in the Arena!\n+%dc +%dxp\nCaps:%d",
            target.name, capGain, xpGain, p.caps);
    } else {
        // Player loses
        uint16_t loss = (uint16_t)(p.caps / 10);
        p.caps = (p.caps > loss) ? (uint16_t)(p.caps - loss) : 0;
        p.alive = 0;
        target.xp   += (uint32_t)xpGain;
        target.caps  = (uint16_t)(target.caps + capGain);
        wlSavePlayer(target);
        snprintf(buf, len,
            "You lost to %s in the Arena!\nLost %dc. Respawn tomorrow.",
            target.name, loss);
    }
}

// ─── Leaderboard ──────────────────────────────────────────────────────────────

static void wlDoLeaderboard(const WLPlayer &p, char *buf, size_t len)
{
    WLPlayer top[8];
    uint32_t cnt = wlTopPlayers(top, 8);

    char out[190] = "=== Wasteland Legends ===\n";
    char line[40];
    uint32_t show = (cnt < 5) ? cnt : 5;
    for (uint32_t i = 0; i < show; i++) {
        snprintf(line, sizeof(line), "%u. %s Lv:%d Kills:%d\n",
            (unsigned)(i + 1),
            top[i].name, top[i].level, top[i].kills);
        strncat(out, line, sizeof(out) - strlen(out) - 1);
    }
    if (cnt == 0) {
        strncat(out, "No survivors yet!", sizeof(out) - strlen(out) - 1);
    }
    snprintf(buf, len, "%s", out);
    (void)p; // suppress unused warning
}

// ─── Main entry point ─────────────────────────────────────────────────────────

void wlCommand(uint32_t nodeNum, const char *text, const char *shortName,
               char *outBuf, size_t outLen)
{
    uint32_t now = (uint32_t)getTime();

    WLPlayer p;
    bool isNew = !wlLoadPlayer(nodeNum, p);
    if (isNew) {
        wlNewPlayer(nodeNum, shortName, p);
    } else {
        // Refresh short name if it changed
        if (shortName && shortName[0]) {
            strncpy(p.name, shortName, 4);
            p.name[4] = '\0';
        }
    }
    wlResetApIfNeeded(p, now);

    // ── Welcome new players ──
    if (isNew) {
        snprintf(outBuf, outLen,
            "Welcome, %s!\n"
            "You emerge from Vault 111.\n"
            "A Deathclaw Alpha terrorizes\n"
            "the wasteland. Prevail?\n"
            "[E]xplore  [X]Back to BBS",
            p.name);
        wlSavePlayer(p);
        return;
    }

    // ── Parse command (case-insensitive, first 2 chars) ──
    char cmd[8] = {0};
    int ci = 0;
    while (text[ci] && ci < 7 && !isspace((unsigned char)text[ci])) {
        cmd[ci] = (char)tolower((unsigned char)text[ci]);
        ci++;
    }
    cmd[ci] = '\0';
    const char *arg = text + ci;
    while (*arg == ' ') arg++;

    // ── Route ──
    if (p.inCombat) {
        // In active combat — restrict to combat commands
        if (cmd[0] == 'a' && cmd[1] != 'r') {
            wlDoAttack(p, outBuf, outLen);
        } else if (cmd[0] == 'd') {
            wlDoDefend(p, outBuf, outLen);
        } else if (cmd[0] == 's' && cmd[1] != 't' && cmd[1] != 'h') {
            // 's' alone = stimpak in combat
            wlDoStimpak(p, outBuf, outLen, true);
        } else if (cmd[0] == 'f') {
            wlDoFlee(p, outBuf, outLen);
        } else if (cmd[0] == 's' && cmd[1] == 't') {
            wlDoStats(p, outBuf, outLen);
        } else if (cmd[0] == 'x' || cmd[0] == '\0') {
            snprintf(outBuf, outLen, "In combat! Use A/D/S/F.\nCan't exit now.");
        } else {
            wlShowCombat(p, outBuf, outLen);
        }
    } else if (cmd[0] == 'e') {
        wlDoExplore(p, outBuf, outLen, now);
    } else if (cmd[0] == 'h') {
        wlDoHeal(p, outBuf, outLen, now);
    } else if (cmd[0] == 's' && cmd[1] == 'h') {
        wlDoShop(p, arg, outBuf, outLen);
    } else if (cmd[0] == 's' && cmd[1] == 't') {
        wlDoStats(p, outBuf, outLen);
    } else if (cmd[0] == 'l') {
        wlDoLeaderboard(p, outBuf, outLen);
    } else if (cmd[0] == 't') {
        wlDoTrain(p, outBuf, outLen);
    } else if (cmd[0] == 'a' && cmd[1] == 'r') {
        wlDoArena(p, arg, outBuf, outLen, now, nodeNum);
    } else if (cmd[0] == 's') {
        // 's' alone out of combat = stimpak
        wlDoStimpak(p, outBuf, outLen, false);
    } else {
        // Unknown command or 'x' / empty → show menu
        wlShowMenu(p, outBuf, outLen);
    }

    wlSavePlayer(p);
}
