// FalloutWastelandRPG.cpp — Fallout Wasteland RPG door game for TinyBBS Vault-Tec Edition
// Part of the Meshtastic BBS module running on ESP32-S3 (Heltec V3).
// All outBuf writes produce ≤200 bytes per single Meshtastic message.
// FSCom rules: use FSCom.open/read/write/close, never fopen/fclose.

#include "FalloutWastelandRPG.h"
#include "BBSPlatform.h"
#include "FSCommon.h"
#include "RTC.h"
#include <Arduino.h>
#include <cctype>
#include <cstring>
#include <cstdio>

// ─── Announce flag (checked by BBSModule after frpgCommand returns) ───────────
bool frpgPendingAnnounce = false;
char frpgAnnounceMsg[120] = {0};

// ─── Game tables (Wastelad themed) ────────────────────────────────────────────

// Weapons from Wastelad (holotape game, Fallout 76)
const FRPGWeapon FRPG_WEAPONS[12] = {
    {"Baseball Bat",    8,  1,     0},
    {"Brass Knuckles", 10,  1,   150},
    {"Machete",        13,  2,   300},
    {"Hunting Rifle",  18,  3,   500},
    {"Dbl-Brl Shotgn", 23,  4,   850},
    {"Laser Pistol",   28,  5,  1300},
    {"Sniper Rifle",   34,  6,  2000},
    {"Stinger Sword",  40,  7,  2800},
    {"Laser Rifle",    48,  8,  3800},
    {"Hydraulic Arm",  57,  9,  5200},
    {"Minigun",        68, 10,  7500},
    {"Gatling Laser",  82, 11, 11000},
};

// Armor from Wastelad (expanded to 10 tiers)
const FRPGArmor FRPG_ARMORS[10] = {
    {"Vault Suit",      0,  1,     0},
    {"Leather Armor",   4,  2,   200},
    {"Armrd VltSuit",   7,  3,   500},
    {"Combat Armor",   11,  4,   900},
    {"Riot Shield",    14,  5,  1500},
    {"Insul VltSuit",  17,  6,  2300},
    {"Rustd PwrArmor", 21,  7,  3400},
    {"T-45a PwrArmor", 26,  8,  5000},
    {"T-60 PwrArmor",  31,  9,  7000},
    {"X-01 PwrArmor",  38, 10, 10000},
};

// ETYPE_HUMANOID=0  ETYPE_CREATURE=1  ETYPE_ROBOT=2
// Enemies from Wastelad lore: 4 tiers, 4 per tier
const FRPGEnemy FRPG_ENEMIES[16] = {
    // Tier 1 — near Vault 1 outskirts
    {"Gnt Centipede",   1, 10,  4, 10, 12, ETYPE_CREATURE},
    {"Mutant Squirrel", 1, 15,  5, 12, 15, ETYPE_CREATURE},
    {"Zombie",          1, 18,  7, 15, 18, ETYPE_HUMANOID},
    {"Raider",          1, 22,  9, 20, 25, ETYPE_HUMANOID},
    // Tier 2 — truck stops, outposts
    {"Irrad Zombie",    2, 30, 12, 35, 42, ETYPE_HUMANOID},
    {"Giant Spider",    2, 35, 13, 38, 46, ETYPE_CREATURE},
    {"Apocal Hornet",   2, 32, 14, 36, 44, ETYPE_CREATURE},
    {"Chinese Soldier", 2, 38, 15, 42, 52, ETYPE_HUMANOID},
    // Tier 3 — radio tower, stadium, armory
    {"Porcu-Possum",    3, 52, 17, 65, 85, ETYPE_CREATURE},
    {"Radr Suicider",   3, 20, 32, 60, 80, ETYPE_HUMANOID},  // glass cannon, high atk
    {"Chinese Grendr",  3, 25, 30, 62, 82, ETYPE_HUMANOID},  // glass cannon, high atk
    {"Chinese Officer", 3, 58, 21, 72, 95, ETYPE_HUMANOID},
    // Tier 4 — Cheng's fortress outskirts
    {"Massive Raider",  4, 82, 26,120,180, ETYPE_HUMANOID},
    {"Mutant Mass",     4, 92, 29,132,202, ETYPE_HUMANOID},
    {"Malf Protectron", 4, 78, 23,126,188, ETYPE_ROBOT},
    {"Military Sentry", 4,105, 32,145,225, ETYPE_ROBOT},
};

// Wastelad boss ladder — each one corresponds to a trainer level (0=lvl1 → 11=lvl12)
// Trainer 11 (Lagoon Monster) is the final ladder boss; beating it unlocks Chairman Cheng
const FRPGTrainer FRPG_TRAINERS[12] = {
    {"Zombie Leader",    80, 15,  200,   80},   //  0 – lvl 1→2
    {"Raider Lt.",      110, 18,  380,  140},   //  1 – lvl 2→3
    {"Chinese Sgt.",    145, 22,  600,  220},   //  2 – lvl 3→4
    {"Failed Exprmnt",  185, 27,  900,  340},   //  3 – lvl 4→5  (alligator-man)
    {"Bat-Bear",        230, 32, 1300,  520},   //  4 – lvl 5→6  (Radio Tower boss)
    {"Brutus Butkus",   285, 37, 1800,  760},   //  5 – lvl 6→7  (Raider King)
    {"Auto Def.Sys.",   345, 34, 2500, 1050},   //  6 – lvl 7→8  (machinegun bank)
    {"Comrade Chrome",  420, 43, 3400, 1450},   //  7 – lvl 8→9  (war robot)
    {"Mutant Warlord",  500, 48, 4500, 1900},   //  8 – lvl 9→10
    {"Goliath",         640, 54, 6000, 2700},   //  9 – lvl 10→11 (wears car armor)
    {"Chng Elite Grd",  500, 47, 7500, 3500},   // 10 – lvl 11→12 (gatekeeper)
    {"Lagoon Monster",  900, 60, 9000, 5000},   // 11 – BOSS: unlocks Chairman Cheng
};

// ─── VATS body-part tables ────────────────────────────────────────────────────
// hitMul10: hitChance% = per * hitMul10 / 10, capped at hitCap

const FRPGVATSPart FRPG_VATS_HUMANOID[] = {
    {"HEAD  [Stun]",    30, 80, LIMB_STUNNED},
    {"TORSO [Expose]",  50, 95, LIMB_EXPOSE},
    {"R.ARM [Disarm]",  40, 90, LIMB_DISARM},
    {"L.ARM [Weaken]",  40, 90, LIMB_WEAKEN},
    {"LEGS  [Cripple]", 45, 90, LIMB_CRIPPLED},
    {nullptr, 0, 0, 0}
};
const FRPGVATSPart FRPG_VATS_CREATURE[] = {
    {"HEAD  [Daze]",    30, 80, LIMB_STUNNED},
    {"TORSO [Expose]",  50, 95, LIMB_EXPOSE},
    {"LEGS  [Slow]",    40, 90, LIMB_CRIPPLED},
    {"TAIL  [Disarm]",  30, 75, LIMB_DISARM},
    {nullptr, 0, 0, 0}
};
const FRPGVATSPart FRPG_VATS_ROBOT[] = {
    {"SENSOR [Blind]",  30, 80, LIMB_BLIND},
    {"CORE   [Expose]", 50, 95, LIMB_EXPOSE},
    {"ARMS   [Disarm]", 40, 90, LIMB_DISARM},
    {"TREADS [Slow]",   40, 90, LIMB_CRIPPLED},
    {"INHIBIT[Frenzy]", 25, 70, LIMB_FRENZY},
    {nullptr, 0, 0, 0}
};

// ─── Hack word list (200 five-letter words) ───────────────────────────────────

const char *FRPG_HACK_WORDS[200] = {
    "ABOUT","ADMIT","AFTER","AGENT","AGREE","AHEAD","ALARM","ALERT","ALIKE","ALTER",
    "ANGEL","ANGLE","ANGRY","ANKLE","APART","APPLY","ARENA","ARGUE","ARISE","ARMOR",
    "ARRAY","ARROW","ASIDE","ASSET","AWARD","AWARE","AWFUL","BASIC","BEACH","BEARD",
    "BEAST","BEGIN","BENCH","BIRTH","BLACK","BLADE","BLANK","BLAST","BLAZE","BLEND",
    "BLIND","BLOCK","BLOOD","BLOWN","BOARD","BOOST","BOUND","BRAIN","BRAKE","BRAND",
    "BRAVE","BRAWL","BREAD","BREAK","BREED","BRING","BROKE","BROWN","BRUSH","BUILT",
    "BUNCH","BURST","CATCH","CAUSE","CHAIN","CHAOS","CHARM","CHASE","CHECK","CHEST",
    "CHIEF","CLAIM","CLASH","CLASS","CLEAN","CLEAR","CLICK","CLIFF","CLIMB","CLOSE",
    "CLOTH","CLOUD","COUNT","COVER","CRACK","CRAFT","CRANE","CRASH","CREAM","CRIME",
    "CRISP","CROSS","CROWD","CRUEL","CRUSH","CURVE","DANCE","DEATH","DECAY","DELAY",
    "DENSE","DEPTH","DEVIL","DOUBT","DRAFT","DRAIN","DRAMA","DRAWN","DREAD","DREAM",
    "DRESS","DRIFT","DRINK","DRIVE","DRONE","DROWN","DRUNK","EAGLE","EARLY","EARTH",
    "ELECT","ELITE","EMPTY","ENEMY","ENTER","EQUAL","ERASE","EVENT","EXACT","EXILE",
    "EXTRA","FAINT","FALSE","FANCY","FEAST","FENCE","FEVER","FIGHT","FINAL","FIRST",
    "FLAME","FLARE","FLASH","FLEET","FLESH","FLINT","FLOOD","FLOOR","FORCE","FORGE",
    "FORTH","FOUND","FRAME","FRANK","FRESH","FROST","FRUIT","GHOUL","GIVEN","GLARE",
    "GLEAM","GLIDE","GLOBE","GLORY","GRANT","GRAVE","GREAT","GREED","GRIEF","GUARD",
    "LASER","LEARN","LEVEL","LIGHT","LIMIT","LOGIC","LUCKY","MAGIC","MATCH","MIGHT",
    "MODEL","MONEY","MORAL","MOUNT","NIGHT","NOBLE","OCCUR","ORDER","POWER","PRICE",
    "PRIDE","PRIME","PROOF","QUEST","QUICK","RADAR","RANGE","RAPID","REBEL","VAULT",
};

// ─── Timezone helpers ─────────────────────────────────────────────────────────

static uint32_t frpgNextMidnightEastern(uint32_t now)
{
    int64_t localNow     = (int64_t)now + FRPG_EASTERN_OFFSET;
    int64_t nextMidnight = ((localNow / 86400) + 1) * 86400;
    return (uint32_t)(nextMidnight - FRPG_EASTERN_OFFSET);
}

// ─── Forward declarations ─────────────────────────────────────────────────────

static void frpgShowMenu(FRPGPlayer &p, char *buf, size_t len);
static void frpgShowCombat(const FRPGPlayer &p, char *buf, size_t len);
static void frpgShowHackBoard(const FRPGHack &h, char *buf, size_t len);
static void frpgDoAttack(FRPGPlayer &p, char *buf, size_t len);
static void frpgDoDefend(FRPGPlayer &p, char *buf, size_t len);
static void frpgDoStimpak(FRPGPlayer &p, char *buf, size_t len);
static void frpgDoFlee(FRPGPlayer &p, char *buf, size_t len);
static void frpgEnterVATS(FRPGPlayer &p, char *buf, size_t len);
static void frpgPickVATS(FRPGPlayer &p, int pick, char *buf, size_t len);
static void frpgStartExplore(FRPGPlayer &p, char *buf, size_t len);
static void frpgHackGuess(FRPGPlayer &p, const char *word, char *buf, size_t len);
static void frpgDoStats(const FRPGPlayer &p, char *buf, size_t len);
static void frpgDoShop(const FRPGPlayer &p, char *buf, size_t len);
static void frpgDoBuy(FRPGPlayer &p, const char *arg, char *buf, size_t len);
static void frpgDoHeal(FRPGPlayer &p, char *buf, size_t len);
static void frpgDoTrain(FRPGPlayer &p, char *buf, size_t len);
static void frpgDoArena(FRPGPlayer &p, const char *arg, char *buf, size_t len);
static void frpgDoBoard(char *buf, size_t len);
static void frpgDoAbout(char *buf, size_t len);
static void frpgDoHelp(const FRPGPlayer &p, char *buf, size_t len);
static void frpgDoTavern(char *buf, size_t len);
static void frpgDoCheng(FRPGPlayer &p, char *buf, size_t len);

// ─── File I/O ─────────────────────────────────────────────────────────────────

void frpgEnsureDir()
{
    if (!FSCom.exists("/bbs"))      FSCom.mkdir("/bbs");
    if (!FSCom.exists(FRPG_DIR))    FSCom.mkdir(FRPG_DIR);
}

static void frpgPath(uint32_t nodeNum, char *path, size_t len)
{
    snprintf(path, len, "%s/p%08x.bin", FRPG_DIR, (unsigned)nodeNum);
}

bool frpgLoadPlayer(uint32_t nodeNum, FRPGPlayer &p)
{
    char path[40];
    frpgPath(nodeNum, path, sizeof(path));
    LOG_DEBUG("[FRPG] load path=%s sizeof=%u\n", path, (unsigned)sizeof(FRPGPlayer));
    (void)0;
    File f = FSCom.open(path, FILE_O_READ);
    if (!f) {
        LOG_DEBUG("[FRPG] load: file not found\n");
        (void)0;
        return false;
    }
    size_t n = f.read((uint8_t *)&p, sizeof(FRPGPlayer));
    f.close();
    LOG_DEBUG("[FRPG] load: read %u bytes (need %u)\n", (unsigned)n, (unsigned)sizeof(FRPGPlayer));
    (void)0;
    return (n == sizeof(FRPGPlayer));
}

void frpgSavePlayer(const FRPGPlayer &p)
{
    char path[40];
    frpgPath(p.nodeNum, path, sizeof(path));
    LOG_DEBUG("[FRPG] save path=%s nodeNum=0x%08x sizeof=%u\n", path, (unsigned)p.nodeNum, (unsigned)sizeof(FRPGPlayer));
    (void)0;
    File f = FSCom.open(path, FILE_O_WRITE);
    if (!f) {
        LOG_DEBUG("[FRPG] save: FAILED to open file\n");
        (void)0;
        return;
    }
    size_t n = f.write((const uint8_t *)&p, sizeof(FRPGPlayer));
    f.close();
    LOG_DEBUG("[FRPG] save: wrote %u bytes\n", (unsigned)n);
    (void)0;
}

void frpgNewPlayer(uint32_t nodeNum, const char *shortName, FRPGPlayer &p)
{
    memset(&p, 0, sizeof(FRPGPlayer));
    p.nodeNum       = nodeNum;
    strncpy(p.name, shortName ? shortName : "Wndlr", 4);
    p.name[4]       = '\0';
    p.level         = 1;
    p.str_          = 5;
    p.per           = 5;
    p.end_          = 5;
    p.intel         = 5;
    p.agi           = 5;
    p.maxHp         = 50;
    p.hp            = 50;
    p.caps          = 100;
    p.stimpaks      = 2;
    p.weapon        = 0;
    p.armor         = 0;
    p.ap            = FRPG_AP_MAX;
    p.vatsMax       = (uint8_t)(2 + p.agi / 4);
    p.vatsCharges   = p.vatsMax;
    p.alive         = 1;
    p.apResetTime   = frpgNextMidnightEastern((uint32_t)getTime());
}

// ─── Daily reset ──────────────────────────────────────────────────────────────

static void frpgResetIfNeeded(FRPGPlayer &p)
{
    uint32_t now = (uint32_t)getTime();
    if (now < p.apResetTime) return;  // not yet midnight Eastern
    p.ap            = FRPG_AP_MAX;
    p.vatsMax       = (uint8_t)(2 + p.agi / 4);
    p.vatsCharges   = p.vatsMax;
    p.hp            = (p.hp < 1) ? 1 : p.hp;  // respawn at 1 HP minimum if dead
    p.alive         = 1;
    p.healedToday   = 0;
    p.lastPvpTarget = 0;
    p.apResetTime   = frpgNextMidnightEastern(now);
}

// ─── Leaderboard ──────────────────────────────────────────────────────────────

uint32_t frpgTopPlayers(FRPGPlayer *out, uint32_t max)
{
    uint32_t count = 0;
    File dir = FSCom.open(FRPG_DIR, FILE_O_READ);
    if (!dir) return 0;
    BBS_FILE_VAR(f);
    while ((f = dir.openNextFile()) && count < max) {
        if (f.isDirectory()) { f.close(); continue; }
        const char *nm = f.name();
        if (!nm || nm[0] != 'p') { f.close(); continue; }
        FRPGPlayer tmp;
        if (f.read((uint8_t *)&tmp, sizeof(tmp)) != sizeof(tmp)) { f.close(); continue; }
        f.close();
        // insertion-sort: chengKills DESC, then kills DESC, then level DESC
        out[count] = tmp;
        for (uint32_t i = count; i > 0; i--) {
            FRPGPlayer &a = out[i], &b = out[i-1];
            bool swap = (a.chengKills > b.chengKills) ||
                        (a.chengKills == b.chengKills && a.kills > b.kills) ||
                        (a.chengKills == b.chengKills && a.kills == b.kills && a.level > b.level);
            if (!swap) break;
            FRPGPlayer t = a; a = b; b = t;
        }
        count++;
    }
    dir.close();
    return count;
}

// ─── Combat helpers ───────────────────────────────────────────────────────────

static uint8_t frpgGetTier(uint8_t level)
{
    int t = (level - 1) / 3 + 1;
    if (t < 1) t = 1;
    if (t > 4) t = 4;
    return (uint8_t)t;
}

static const char *frpgEnemyName(const FRPGPlayer &p)
{
    if (p.combat.active == 4) return "Chairman Cheng";
    if (p.combat.active == 2) return FRPG_TRAINERS[p.combat.enemyIdx].name;
    return FRPG_ENEMIES[p.combat.enemyIdx].name;
}

// Base player attack damage (no VATS EXPOSE yet applied).
static int frpgPlayerDmg(const FRPGPlayer &p)
{
    int dmg = (int)FRPG_WEAPONS[p.weapon].damage
              + (int)p.str_ / 2
              + (int)random(p.str_ / 2 + 1);
    if ((int)random(100) < (int)p.per) dmg *= 2; // critical
    return dmg;
}

// Effective enemy damage to player (after all reductions, armor, defend).
static int frpgEnemyDmg(const FRPGPlayer &p)
{
    if (p.combat.limbEffects & LIMB_STUNNED) return 0; // stunned = no dmg
    int atk = (int)p.combat.baseEnemyAtk;
    // CRIPPLED (legs): creature/humanoid deal 30% less
    if (p.combat.limbEffects & LIMB_CRIPPLED) atk = atk * 70 / 100;
    // BLIND (robot): deal 30% less
    if (p.combat.limbEffects & LIMB_BLIND)    atk = atk * 70 / 100;
    int dmg = atk + (int)random(atk / 2 + 1)
              - (int)FRPG_ARMORS[p.armor].defense;
    if (p.combat.defending) dmg /= 2;
    if (dmg < 1) dmg = 1;
    return dmg;
}

// Apply player damage with EXPOSE bonus.
static int frpgPlayerDmgFull(const FRPGPlayer &p)
{
    int dmg = frpgPlayerDmg(p);
    if (p.combat.limbEffects & LIMB_EXPOSE) {
        dmg = dmg * (p.combat.critExpose ? 150 : 125) / 100;
    }
    return dmg;
}

// Decrement stun after it blocks an attack.
static void frpgTickStun(FRPGPlayer &p)
{
    if (p.combat.limbEffects & LIMB_STUNNED) {
        if (p.combat.stunRounds > 0) p.combat.stunRounds--;
        if (p.combat.stunRounds == 0) p.combat.limbEffects &= ~LIMB_STUNNED;
    }
}

// Build compact limb-effect annotation, e.g. " [STUN CRIPPLE]"
static void frpgEffectStr(const FRPGPlayer &p, char *out, size_t len)
{
    uint8_t fx = p.combat.limbEffects;
    if (!fx) { out[0] = '\0'; return; }
    char tmp[64] = " [";
    int pos = 2;
    if (fx & LIMB_STUNNED)  pos += snprintf(tmp+pos, sizeof(tmp)-pos, "STUN ");
    if (fx & LIMB_EXPOSE)   pos += snprintf(tmp+pos, sizeof(tmp)-pos, "XPOSE ");
    if (fx & LIMB_DISARM)   pos += snprintf(tmp+pos, sizeof(tmp)-pos, "DISARM ");
    if (fx & LIMB_WEAKEN)   pos += snprintf(tmp+pos, sizeof(tmp)-pos, "WEAK ");
    if (fx & LIMB_CRIPPLED) pos += snprintf(tmp+pos, sizeof(tmp)-pos, "CRIP ");
    if (fx & LIMB_BLIND)    pos += snprintf(tmp+pos, sizeof(tmp)-pos, "BLIND ");
    if (fx & LIMB_FRENZY)   pos += snprintf(tmp+pos, sizeof(tmp)-pos, "FRNZY ");
    // trim trailing space
    while (pos > 2 && tmp[pos-1] == ' ') pos--;
    tmp[pos++] = ']';
    tmp[pos] = '\0';
    strncpy(out, tmp, len-1);
    out[len-1] = '\0';
}

static void frpgVictory(FRPGPlayer &p, int pdmg, bool isCrit,
                        const char *critLabel, char *buf, size_t len)
{
    bool isCheng   = (p.combat.active == 4);
    bool isTrainer = (p.combat.active == 2);
    bool isBoss    = (isTrainer && p.combat.enemyIdx == FRPG_TRAINER_COUNT - 1);
    uint8_t savedIdx = p.combat.enemyIdx;

    p.combat.active       = 0;
    p.combat.limbEffects  = 0;
    p.combat.stunRounds   = 0;
    p.combat.vatsModeActive = 0;
    p.combat.defending    = 0;

    if (isCheng) {
        p.chengKills++;
        uint16_t capGain = 5000;
        if ((uint32_t)p.caps + capGain > 9999) capGain = (uint16_t)(9999 - p.caps);
        p.caps = (uint16_t)(p.caps + capGain);
        p.xp  += 10000;
        frpgPendingAnnounce = true;
        snprintf(frpgAnnounceMsg, sizeof(frpgAnnounceMsg),
            "[Wastelad] %s defeated Chairman Cheng! The Wasteland is saved! 73!",
            p.name);
        snprintf(buf, len,
            "CHENG DEFEATED!%s\n"
            "The Wasteland is saved!\n"
            "Cheng Kills:%d\n"
            "+%dc +10000xp\nHP:%d/%d",
            critLabel, p.chengKills, capGain, p.hp, p.maxHp);
    } else if (isBoss) {
        uint16_t xg = FRPG_TRAINERS[savedIdx].xpReward;
        uint16_t cg = FRPG_TRAINERS[savedIdx].capReward;
        p.xp   += xg;
        p.caps  = (uint16_t)(p.caps + cg);
        p.kills++;
        snprintf(buf, len,
            "LAGOON MONSTER SLAIN!%s\n"
            "Boss Kills:%d\n"
            "+%dc +%uxp\nHP:%d/%d\n"
            "Use CH to face Cheng!",
            critLabel, p.kills, cg, (unsigned)xg, p.hp, p.maxHp);
    } else if (isTrainer) {
        uint16_t xg = FRPG_TRAINERS[savedIdx].xpReward;
        uint16_t cg = FRPG_TRAINERS[savedIdx].capReward;
        p.xp   += xg;
        p.caps  = (uint16_t)(p.caps + cg);
        p.level++;
        p.maxHp = (uint16_t)(p.maxHp + 10 + p.end_);
        p.hp    = p.maxHp;
        p.str_  = (uint8_t)(p.str_ + 2);
        p.per   = (uint8_t)(p.per  + 2);
        p.end_  = (uint8_t)(p.end_ + 1);
        p.intel = (uint8_t)(p.intel + 1);
        p.agi   = (uint8_t)(p.agi   + 1);
        p.vatsMax = (uint8_t)(2 + p.agi / 4);
        snprintf(buf, len,
            "%s defeated!%s\nLEVEL UP! Lvl:%d\n"
            "HP:%d S:%d P:%d E:%d I:%d A:%d\n"
            "+%dc +%uxp",
            FRPG_TRAINERS[savedIdx].name, critLabel,
            p.level, p.maxHp, p.str_, p.per, p.end_, p.intel, p.agi,
            cg, (unsigned)xg);
    } else {
        uint16_t xg = FRPG_ENEMIES[savedIdx].xpReward;
        uint16_t cg = FRPG_ENEMIES[savedIdx].capReward;
        p.xp   += xg;
        p.caps  = (uint16_t)(p.caps + cg);
        snprintf(buf, len,
            "%s defeated!%s\n+%uxp +%dc\nHP:%d/%d AP:%d/%d",
            FRPG_ENEMIES[savedIdx].name, critLabel,
            (unsigned)xg, cg, p.hp, p.maxHp, p.ap, FRPG_AP_MAX);
    }
}

static void frpgDeath(FRPGPlayer &p, const char *eName, char *buf, size_t len)
{
    p.hp    = 0;
    p.alive = 0;
    uint16_t loss = (uint16_t)(p.caps / 5);
    p.caps = (p.caps > loss) ? (uint16_t)(p.caps - loss) : 0;
    p.combat.active      = 0;
    p.combat.limbEffects = 0;
    p.combat.vatsModeActive = 0;
    p.combat.defending   = 0;
    p.hack.active        = 0;
    snprintf(buf, len,
        "%s killed you!\nLost %dc. Respawn tomorrow.\nSH/ST/LB available.",
        eName, loss);
}

// ─── VATS ─────────────────────────────────────────────────────────────────────

static const FRPGVATSPart *frpgVATSTable(uint8_t etype, int &nParts)
{
    switch (etype) {
        case ETYPE_CREATURE: nParts = 4; return FRPG_VATS_CREATURE;
        case ETYPE_ROBOT:    nParts = 5; return FRPG_VATS_ROBOT;
        default:             nParts = 5; return FRPG_VATS_HUMANOID;
    }
}

// Deterministic per-part jitter from the stored seed: returns -12..+12
static int frpgVatsJitter(uint8_t seed, int partIdx)
{
    return (int)((seed * (uint8_t)(partIdx * 37 + 17)) % 25u) - 12;
}

static void frpgEnterVATS(FRPGPlayer &p, char *buf, size_t len)
{
    if (p.vatsCharges == 0) {
        snprintf(buf, len, "V.A.T.S. unavailable!\nNo charges today.\nA/D/S/F?");
        return;
    }
    int nParts;
    const FRPGVATSPart *parts = frpgVATSTable(p.combat.enemyType, nParts);
    p.combat.vatsModeActive = 1;
    // Fresh seed each time VATS is opened so percentages vary
    p.combat._pad = (uint8_t)random(256);

    char tmp[200];
    int pos = snprintf(tmp, sizeof(tmp),
        "=== V.A.T.S. ===\n%s PER:%d\n",
        frpgEnemyName(p), p.per);
    for (int i = 0; i < nParts && parts[i].hitMul10; i++) {
        int hc = 40 + (int)p.per * parts[i].hitMul10 / 10
                    + frpgVatsJitter(p.combat._pad, i);
        if (hc < 10) hc = 10;
        if (hc > parts[i].hitCap) hc = parts[i].hitCap;
        pos += snprintf(tmp+pos, sizeof(tmp)-pos,
            "%d)%s %d%%\n", i+1, parts[i].label, hc);
    }
    snprintf(tmp+pos, sizeof(tmp)-pos, "Chg:%d/%d | 1-%d or C",
             p.vatsCharges, p.vatsMax, nParts);
    strncpy(buf, tmp, len-1);
    buf[len-1] = '\0';
}

static void frpgPickVATS(FRPGPlayer &p, int pick0, char *buf, size_t len)
{
    // pick0 is 0-indexed
    int nParts;
    const FRPGVATSPart *parts = frpgVATSTable(p.combat.enemyType, nParts);
    p.combat.vatsModeActive = 0;

    if (pick0 < 0 || pick0 >= nParts) {
        snprintf(buf, len, "Invalid target.\nPick 1-%d or C.", nParts);
        p.combat.vatsModeActive = 1;
        return;
    }

    p.vatsCharges--;
    const FRPGVATSPart &part = parts[pick0];

    // Tick any stun from the PREVIOUS round before applying new effects
    frpgTickStun(p);

    int hc = 40 + (int)p.per * part.hitMul10 / 10
                + frpgVatsJitter(p.combat._pad, pick0);
    if (hc < 10) hc = 10;
    if (hc > part.hitCap) hc = part.hitCap;
    bool hit = ((int)random(100) < hc);

    const char *eName = frpgEnemyName(p);

    if (!hit) {
        // Miss — enemy attacks back
        int edamg = frpgEnemyDmg(p);
        p.combat.defending = 0;
        const char *stunLabel = (p.combat.limbEffects & LIMB_STUNNED) ? " STUNNED!" : "";
        if (edamg > 0 && p.hp <= (uint16_t)edamg) {
            frpgDeath(p, eName, buf, len);
        } else {
            if (edamg > 0) p.hp -= (uint16_t)edamg;
            char fxStr[32]; frpgEffectStr(p, fxStr, sizeof(fxStr));
            snprintf(buf, len,
                "V.A.T.S MISS!%s\nChg:%d/%d\n%s hits %d%s\nHP:%d/%d | A/D/S/F/V",
                stunLabel, p.vatsCharges, p.vatsMax,
                eName, edamg, fxStr, p.hp, p.maxHp);
        }
        return;
    }

    // Hit — check for VATS critical (higher chance than normal attack)
    int critChance = 10 + (int)p.per * 3;
    if (critChance > 45) critChance = 45;
    bool isCrit = ((int)random(100) < critChance);
    const char *critLabel = isCrit ? " CRIT!" : "";

    // Apply limb effect
    char effectMsg[60];
    switch (part.effect) {
        case LIMB_STUNNED:
            p.combat.limbEffects |= LIMB_STUNNED;
            p.combat.stunRounds   = isCrit ? 2 : 1;
            snprintf(effectMsg, sizeof(effectMsg), "%s%s", "STUNNED!", critLabel);
            break;
        case LIMB_EXPOSE:
            p.combat.limbEffects |= LIMB_EXPOSE;
            p.combat.critExpose   = isCrit ? 1 : 0;
            snprintf(effectMsg, sizeof(effectMsg), "EXPOSED! +%d%% dmg%s",
                     isCrit ? 50 : 25, critLabel);
            break;
        case LIMB_DISARM: {
            p.combat.limbEffects |= LIMB_DISARM;
            int pct = isCrit ? 80 : 40;
            p.combat.baseEnemyAtk = (uint8_t)(p.combat.baseEnemyAtk * (100-pct) / 100);
            if (p.combat.baseEnemyAtk < 1) p.combat.baseEnemyAtk = 1;
            snprintf(effectMsg, sizeof(effectMsg), "DISARMED! -%d%% atk%s", pct, critLabel);
            break;
        }
        case LIMB_WEAKEN: {
            p.combat.limbEffects |= LIMB_WEAKEN;
            int pct = isCrit ? 50 : 25;
            p.combat.baseEnemyAtk = (uint8_t)(p.combat.baseEnemyAtk * (100-pct) / 100);
            if (p.combat.baseEnemyAtk < 1) p.combat.baseEnemyAtk = 1;
            snprintf(effectMsg, sizeof(effectMsg), "WEAKENED! -%d%% atk%s", pct, critLabel);
            break;
        }
        case LIMB_CRIPPLED:
            p.combat.limbEffects |= LIMB_CRIPPLED;
            snprintf(effectMsg, sizeof(effectMsg), "CRIPPLED!%s Flee+%s", critLabel,
                     isCrit ? "95%" : "90%");
            break;
        case LIMB_BLIND:
            p.combat.limbEffects |= LIMB_BLIND;
            snprintf(effectMsg, sizeof(effectMsg), "BLINDED! -%d%% dmg%s",
                     isCrit ? 60 : 30, critLabel);
            break;
        case LIMB_FRENZY:
            p.combat.limbEffects |= LIMB_FRENZY;
            snprintf(effectMsg, sizeof(effectMsg), "FRENZY! Hits itself!%s", critLabel);
            break;
        default:
            effectMsg[0] = '\0';
            break;
    }

    // Deal weapon damage — VATS hits harder than a normal attack (150%)
    int pdmg = frpgPlayerDmgFull(p) * 3 / 2;
    bool enemyDead = ((int)pdmg >= (int)p.combat.enemyHp);
    if (enemyDead) {
        p.combat.enemyHp = 0;
    } else {
        p.combat.enemyHp -= (uint16_t)pdmg;
    }

    if (enemyDead) {
        frpgVictory(p, pdmg, isCrit, critLabel, buf, len);
        return;
    }

    // Enemy counter-attack (handle FRENZY: enemy damages itself)
    p.combat.defending = 0;
    if (p.combat.limbEffects & LIMB_FRENZY) {
        p.combat.limbEffects &= ~LIMB_FRENZY;
        int selfDmg = (int)p.combat.baseEnemyAtk;
        if (selfDmg >= (int)p.combat.enemyHp) {
            p.combat.enemyHp = 0;
            frpgVictory(p, 0, false, "", buf, len);
            return;
        }
        p.combat.enemyHp -= (uint16_t)selfDmg;
        char fxStr[32]; frpgEffectStr(p, fxStr, sizeof(fxStr));
        snprintf(buf, len,
            "V.A.T.S: %s%s\nDMG:%d | %s\n"
            "%s hits self for %d!\nEHP:%d%s | A/D/S/F/V",
            part.label, critLabel, pdmg, effectMsg,
            eName, selfDmg, p.combat.enemyHp, fxStr);
        return;
    }

    int edamg = frpgEnemyDmg(p);
    // edamg==0 if enemy was stunned (frpgEnemyDmg returns 0 when LIMB_STUNNED)
    const char *eAtk = (edamg == 0) ? " STUNNED!" : "";

    if (edamg > 0 && p.hp <= (uint16_t)edamg) {
        p.hp = 0;
        frpgDeath(p, eName, buf, len);
        return;
    }
    if (edamg > 0) p.hp -= (uint16_t)edamg;
    char fxStr[32]; frpgEffectStr(p, fxStr, sizeof(fxStr));
    snprintf(buf, len,
        "V.A.T.S: %s%s\nDMG:%d | %s\n"
        "%s hits %d%s%s\nHP:%d | A/D/S/F/V",
        part.label, critLabel, pdmg, effectMsg,
        eName, edamg, eAtk, fxStr, p.hp);
}

// ─── Combat actions ───────────────────────────────────────────────────────────

static void frpgShowCombat(const FRPGPlayer &p, char *buf, size_t len)
{
    char fxStr[32]; frpgEffectStr(p, fxStr, sizeof(fxStr));
    snprintf(buf, len,
        "Still vs %s\nEHP:%d%s\nYour HP:%d/%d\nA/D/S/F/V(%d)",
        frpgEnemyName(p), p.combat.enemyHp, fxStr,
        p.hp, p.maxHp, p.vatsCharges);
}

static void frpgDoAttack(FRPGPlayer &p, char *buf, size_t len)
{
    const char *eName = frpgEnemyName(p);
    bool miss = ((int)random(100) >= (int)(80 + p.per > 95 ? 95 : 80 + p.per));
    p.combat.defending = 0;

    if (miss) {
        frpgTickStun(p);
        int edamg = frpgEnemyDmg(p);
        if (edamg > 0 && p.hp <= (uint16_t)edamg) {
            frpgDeath(p, eName, buf, len);
        } else {
            if (edamg > 0) p.hp -= (uint16_t)edamg;
            char fxStr[32]; frpgEffectStr(p, fxStr, sizeof(fxStr));
            snprintf(buf, len,
                "Miss! %s hits %d%s\nEHP:%d | HP:%d/%d\nA/D/S/F/V(%d)",
                eName, edamg, fxStr,
                p.combat.enemyHp, p.hp, p.maxHp, p.vatsCharges);
        }
        return;
    }

    int pdmg = frpgPlayerDmgFull(p);
    const char *critLabel = "";
    // Critical detection (embedded in frpgPlayerDmg, but we show it here via heuristic)
    // Simple approach: check if dmg >= 2× base weapon dmg (rough crit indicator)
    if (pdmg >= FRPG_WEAPONS[p.weapon].damage * 2) critLabel = " CRIT!";

    if ((int)pdmg >= (int)p.combat.enemyHp) {
        p.combat.enemyHp = 0;
        frpgVictory(p, pdmg, false, critLabel, buf, len);
        return;
    }
    p.combat.enemyHp -= (uint16_t)pdmg;

    frpgTickStun(p);
    int edamg = frpgEnemyDmg(p);
    if (edamg > 0 && p.hp <= (uint16_t)edamg) {
        p.hp = 0;
        frpgDeath(p, eName, buf, len);
        return;
    }
    if (edamg > 0) p.hp -= (uint16_t)edamg;
    static const char *attackVerb[] = {
        "swing",   // Baseball Bat
        "punch",   // Brass Knuckles
        "slash",   // Machete
        "fire",    // Hunting Rifle
        "blast",   // Dbl-Brl Shotgn
        "fire",    // Laser Pistol
        "fire",    // Sniper Rifle
        "slash",   // Stinger Sword
        "fire",    // Laser Rifle
        "strike",  // Hydraulic Arm
        "fire",    // Minigun
        "fire",    // Gatling Laser
    };
    const char *verb = (p.weapon < 12) ? attackVerb[p.weapon] : "fire";
    char fxStr[32]; frpgEffectStr(p, fxStr, sizeof(fxStr));
    snprintf(buf, len,
        "You %s %s!%s DMG:%d\n%s HP:%d%s\n%s hits %d | HP:%d/%d\nA/D/S/F/V(%d)",
        verb, FRPG_WEAPONS[p.weapon].name, critLabel, pdmg,
        eName, p.combat.enemyHp, fxStr,
        eName, edamg, p.hp, p.maxHp, p.vatsCharges);
}

static void frpgDoDefend(FRPGPlayer &p, char *buf, size_t len)
{
    p.combat.defending = 1;
    const char *eName = frpgEnemyName(p);
    frpgTickStun(p);
    int edamg = frpgEnemyDmg(p); // halved by defending flag
    if (edamg > 0 && p.hp <= (uint16_t)edamg) {
        frpgDeath(p, eName, buf, len);
        return;
    }
    if (edamg > 0) p.hp -= (uint16_t)edamg;
    p.combat.defending = 0;
    char fxStr[32]; frpgEffectStr(p, fxStr, sizeof(fxStr));
    snprintf(buf, len,
        "You brace for impact!\n%s hits %d (halved)%s\nHP:%d/%d\nA/D/S/F/V(%d)",
        eName, edamg, fxStr, p.hp, p.maxHp, p.vatsCharges);
}

static void frpgDoStimpak(FRPGPlayer &p, char *buf, size_t len)
{
    if (p.stimpaks == 0) {
        snprintf(buf, len, "No stimpaks!\nA/D/F/V(%d)?", p.vatsCharges);
        return;
    }
    p.stimpaks--;
    int heal = 30 + p.level * 3;
    p.hp = (uint16_t)(p.hp + heal);
    if (p.hp > p.maxHp) p.hp = p.maxHp;

    if (p.combat.active) {
        // Enemy attacks this round
        const char *eName = frpgEnemyName(p);
        frpgTickStun(p);
        int edamg = frpgEnemyDmg(p);
        if (edamg > 0 && p.hp <= (uint16_t)edamg) {
            frpgDeath(p, eName, buf, len);
            return;
        }
        if (edamg > 0) p.hp -= (uint16_t)edamg;
        p.combat.defending = 0;
        snprintf(buf, len,
            "Stimpak! +%d HP\n%s hits %d\nHP:%d/%d Stim:%d\nA/D/S/F/V(%d)",
            heal, eName, edamg, p.hp, p.maxHp, p.stimpaks, p.vatsCharges);
    } else {
        snprintf(buf, len, "Stimpak! +%d HP\nHP:%d/%d  Stim:%d",
                 heal, p.hp, p.maxHp, p.stimpaks);
    }
}

static void frpgDoFlee(FRPGPlayer &p, char *buf, size_t len)
{
    if (p.combat.active == 3) { // security bot: no flee
        snprintf(buf, len, "Can't flee!\nSecurity lockdown.\nA/D/S?");
        return;
    }
    const char *eName = frpgEnemyName(p);
    p.combat.defending = 0;

    int fleeChance = 50 + (int)p.agi * 2;
    if (fleeChance > 85) fleeChance = 85;
    if (p.combat.limbEffects & LIMB_CRIPPLED) fleeChance = 92; // enemy legs crippled

    if ((int)random(100) < fleeChance) {
        // Success
        p.combat.active      = 0;
        p.combat.limbEffects = 0;
        p.combat.stunRounds  = 0;
        p.combat.vatsModeActive = 0;
        snprintf(buf, len, "Escaped! AP:%d/%d HP:%d/%d",
                 p.ap, FRPG_AP_MAX, p.hp, p.maxHp);
    } else {
        // Failed — enemy free hit
        frpgTickStun(p);
        int edamg = frpgEnemyDmg(p);
        if (edamg > 0 && p.hp <= (uint16_t)edamg) {
            frpgDeath(p, eName, buf, len);
        } else {
            if (edamg > 0) p.hp -= (uint16_t)edamg;
            snprintf(buf, len,
                "FLEE FAILED!\n%s hits %d\nHP:%d/%d\nA/D/S/F/V(%d)",
                eName, edamg, p.hp, p.maxHp, p.vatsCharges);
        }
    }
}

// ─── Hacking minigame ─────────────────────────────────────────────────────────

static int hackSim(const char *a, const char *b)
{
    int m = 0;
    for (int i = 0; i < 5; i++) if (a[i] == b[i]) m++;
    return m;
}

static void hackPickWords(uint8_t difficulty, FRPGHack &h)
{
    uint8_t target = (uint8_t)random(FRPG_HACK_WORD_COUNT);
    const char *tw = FRPG_HACK_WORDS[target];

    int minSim, maxSim;
    switch (difficulty) {
        case 0:  minSim=0; maxSim=1; break; // Novice
        case 1:  minSim=1; maxSim=2; break; // Advanced
        case 2:  minSim=2; maxSim=3; break; // Expert
        default: minSim=3; maxSim=4; break; // Master
    }

    uint8_t candidates[200];
    uint8_t nCand = 0;
    for (uint8_t i = 0; i < FRPG_HACK_WORD_COUNT; i++) {
        if (i == target) continue;
        int sim = hackSim(tw, FRPG_HACK_WORDS[i]);
        if (sim >= minSim && sim <= maxSim) candidates[nCand++] = i;
    }
    // Fallback: pad with any available words
    if (nCand < 11) {
        for (uint8_t i = 0; i < FRPG_HACK_WORD_COUNT && nCand < 11+100; i++) {
            if (i == target) continue;
            bool dup = false;
            for (uint8_t j = 0; j < nCand; j++) if (candidates[j] == i) { dup=true; break; }
            if (!dup) candidates[nCand++] = i;
        }
    }

    // Reservoir sample 11 from candidates
    uint8_t chosen[11];
    uint8_t nChosen = 0;
    for (uint8_t i = 0; i < nCand; i++) {
        if (nChosen < 11) {
            chosen[nChosen++] = candidates[i];
        } else {
            uint8_t j = (uint8_t)random(i + 1);
            if (j < 11) chosen[j] = candidates[i];
        }
    }

    uint8_t pos = (uint8_t)random(12);
    h.targetIdx = pos;
    uint8_t di = 0;
    for (uint8_t i = 0; i < 12; i++) {
        h.words[i] = (i == pos) ? target : chosen[di < 11 ? di++ : 0];
    }
    h.visible = 0x0FFF; // all 12 visible
}

static void hackApplyINT(const FRPGPlayer &p, FRPGHack &h)
{
    int eliminate = 0;
    if      (p.intel >= 15) { eliminate = 4; h.attempts++; }
    else if (p.intel >= 10)   eliminate = 4;
    else if (p.intel >= 5)    eliminate = 2;

    for (int i = 0; i < eliminate; i++) {
        for (int tries = 0; tries < 20; tries++) {
            uint8_t j = (uint8_t)random(12);
            if (j != h.targetIdx && (h.visible & (1u << j))) {
                h.visible &= ~(1u << j);
                break;
            }
        }
    }
}

static void frpgShowHackBoard(const FRPGHack &h, char *buf, size_t len)
{
    static const char *diffLabel[] = {"NOVICE","ADVNCD","EXPERT","MASTER"};
    int pos = snprintf(buf, len, "%s | %d tries\n",
                       diffLabel[h.difficulty < 4 ? h.difficulty : 0],
                       h.attempts);
    int col = 0;
    for (int i = 0; i < 12 && (size_t)pos < len-8; i++) {
        if (!(h.visible & (1u << i))) continue;
        if (col > 0 && col % 4 == 0) { buf[pos++] = '\n'; col = 0; }
        if (col > 0) buf[pos++] = ' ';
        pos += snprintf(buf+pos, len-pos, "%s", FRPG_HACK_WORDS[h.words[i]]);
        col++;
    }
    if (col > 0) buf[pos++] = '\n';
    snprintf(buf+pos, len-pos, "Guess a word or SKIP");
}

static void frpgHackGuess(FRPGPlayer &p, const char *rawWord, char *buf, size_t len)
{
    // Uppercase the guess
    char word[6] = {0};
    for (int i = 0; i < 5 && rawWord[i]; i++)
        word[i] = (char)toupper((unsigned char)rawWord[i]);
    word[5] = '\0';

    // Check it's a valid word in the displayed list
    int guessIdx = -1;
    for (int i = 0; i < 12; i++) {
        if ((p.hack.visible & (1u << i)) &&
            strcmp(word, FRPG_HACK_WORDS[p.hack.words[i]]) == 0) {
            guessIdx = i;
            break;
        }
    }
    if (guessIdx < 0) {
        // Not in list — try to accept if it's exactly 5 alpha chars and compute sim
        bool isWord = (strlen(word) == 5);
        for (int i = 0; i < 5 && isWord; i++) if (!isalpha((unsigned char)word[i])) isWord = false;
        if (!isWord) {
            frpgShowHackBoard(p.hack, buf, len);
            return;
        }
    }

    // Correct answer?
    const char *target = FRPG_HACK_WORDS[p.hack.words[p.hack.targetIdx]];
    if (strcmp(word, target) == 0) {
        // SUCCESS
        p.hack.active = 0;
        int capReward = 0, xpReward = 0;
        bool extraStim = false, statBoost = false;
        switch (p.hack.difficulty) {
            case 0: capReward=20+(int)random(31); xpReward=10;  extraStim=((int)random(3)==0); break;
            case 1: capReward=50+(int)random(101); xpReward=20; extraStim=true; break;
            case 2: capReward=100+(int)random(201); xpReward=40; extraStim=true; statBoost=((int)random(3)==0); break;
            default:capReward=200+(int)random(301); xpReward=60; extraStim=true; statBoost=true; break;
        }
        p.caps = (uint16_t)(p.caps + capReward);
        p.xp  += (uint32_t)xpReward;
        if (extraStim && p.stimpaks < FRPG_STIMPAK_MAX) p.stimpaks++;
        if (statBoost) {
            // Random stat boost
            uint8_t *stats[] = {&p.str_, &p.per, &p.end_, &p.intel, &p.agi};
            (*stats[random(5)])++;
        }
        snprintf(buf, len,
            "> ACCESS GRANTED <\nTerminal unlocked!\n+%dc +%uxp%s%s",
            capReward, (unsigned)xpReward,
            extraStim ? " +Stim" : "",
            statBoost ? " +1stat!" : "");
        return;
    }

    // Wrong — show similarity
    int sim = hackSim(word, target);
    p.hack.attempts--;

    if (p.hack.attempts == 0) {
        // FAILURE — trigger security bot
        p.hack.active = 0;
        // Pick a robot enemy appropriate to player level (Wastelad robots)
        uint8_t secIdx;
        if (p.level <= 6)  secIdx = 14;  // Malf Protectron
        else               secIdx = 15;  // Military Sentry Bot

        p.combat.active      = 3; // security bot, no flee
        p.combat.enemyIdx    = secIdx;
        p.combat.enemyType   = ETYPE_ROBOT;
        uint16_t eHp = (uint16_t)(FRPG_ENEMIES[secIdx].baseHp + p.level * 5);
        uint8_t  eAtk= (uint8_t)(FRPG_ENEMIES[secIdx].baseAtk + p.level * 2);
        p.combat.enemyHp     = eHp;
        p.combat.baseEnemyAtk= eAtk;
        p.combat.limbEffects = 0;
        p.combat.stunRounds  = 0;
        p.combat.vatsModeActive = 0;
        p.combat.defending   = 0;
        snprintf(buf, len,
            "> LOCKOUT <\nAlarm triggered!\n%s appears! HP:%d ATK:%d\nNo fleeing! A/D/S?",
            FRPG_ENEMIES[secIdx].name, eHp, eAtk);
        return;
    }

    // Still going
    char board[200];
    frpgShowHackBoard(p.hack, board, sizeof(board));
    snprintf(buf, len, "%s - %d/5 correct\n%d tries left\n", word, sim, p.hack.attempts);
    // Append board if fits
    int used = (int)strlen(buf);
    int remain = (int)len - used - 1;
    if (remain > 0) {
        strncat(buf, board, remain);
    }
}

// ─── Explore ──────────────────────────────────────────────────────────────────

static void frpgStartExplore(FRPGPlayer &p, char *buf, size_t len)
{
    if (!p.alive) { snprintf(buf, len, "You are dead!\nRest until tomorrow."); return; }
    if (p.ap == 0) { snprintf(buf, len, "No AP left.\nRest until tomorrow."); return; }
    p.ap--;
    p.combat.defending = 0;

    int roll = (int)random(100);

    // 20% random event
    if (roll < 20) {
        switch (random(5)) {
            case 0:
                if (p.stimpaks < FRPG_STIMPAK_MAX) {
                    p.stimpaks++;
                    snprintf(buf, len, "Lucky find! +1 Stimpak\nStim:%d AP:%d/%d", p.stimpaks, p.ap, FRPG_AP_MAX);
                } else {
                    p.caps += 25;
                    snprintf(buf, len, "Found Stimpak (full), sold!\n+25c Caps:%d AP:%d/%d", p.caps, p.ap, FRPG_AP_MAX);
                }
                break;
            case 1: {
                int found = 15 + p.level * 10;
                p.caps = (uint16_t)(p.caps + found);
                snprintf(buf, len, "Scavenged a cache!\n+%dc Caps:%d AP:%d/%d", found, p.caps, p.ap, FRPG_AP_MAX);
                break;
            }
            case 2: {
                int dmg = 5 + p.level;
                p.hp = (p.hp > (uint16_t)dmg) ? (uint16_t)(p.hp - dmg) : 1;
                snprintf(buf, len, "Radiation storm!-%d HP\nHP:%d/%d AP:%d/%d", dmg, p.hp, p.maxHp, p.ap, FRPG_AP_MAX);
                break;
            }
            case 3: {
                // Traveling merchant - sell a cheap stimpak
                snprintf(buf, len, "Traveling merchant!\nStimpak: 30c\nBUY S or ignore.\nAP:%d/%d", p.ap, FRPG_AP_MAX);
                break;
            }
            default: {
                static const char *sights[] = {
                    "Chem addict mumbles\nabout pre-war baseball\nand shuffles off.",
                    "Scrawled on a wall:\n'VAULT 101 WAS LIE'\nSomeone underlined it.",
                    "A Raider corpse with\na note: 'told ya so'\nPinned to the chest.",
                    "Bloatfly buzzes past.\nYou hold your breath.\nIt moves on.",
                    "Old Protectron rolls\nby on patrol.\n'HAVE A NICE DAY.'",
                    "A rusted tin labeled\nPork n' Beans. Empty.\nAlways empty.",
                    "Dust devil kicks up\nnewspaper: WAR ENDS TODAY\n(From 2077.)",
                    "Feral cat stares at\nyou from the rubble.\nPure judgment.",
                    "Skeleton in lawn chair\numbrella drink in hand.\nWasted the end right.",
                    "Nuka-Cola machine\npowers up. Vends nothing.\nPowers back down.",
                    "Gang tag on the wall:\n'RAIDERS RULE THIS BLOCK'\nYou disagree silently.",
                    "Somewhere a radio\nplays Big Iron faintly.\nThen static. Silence.",
                };
                uint8_t si = (uint8_t)(random(12));
                snprintf(buf, len, "%s\nAP:%d/%d", sights[si], p.ap, FRPG_AP_MAX);
                break;
            }
        }
        return;
    }

    // 15% terminal hack (if level >= 2)
    if (roll < 35 && p.level >= 2) {
        uint8_t diff;
        if      (p.level <= 3)  diff = 0;
        else if (p.level <= 6)  diff = 1;
        else if (p.level <= 9)  diff = 2;
        else                    diff = 3;

        p.hack.active     = 1;
        p.hack.attempts   = (diff == 3) ? 3 : 4;
        p.hack.difficulty = diff;
        hackPickWords(diff, p.hack);
        hackApplyINT(p, p.hack);

        static const char *diffName[] = {"NOVICE","ADVANCED","EXPERT","MASTER"};
        static const char *reward[]   = {"Caps+Stim","Rare Loot","Stat Boost","Unique+"};
        char board[160];
        frpgShowHackBoard(p.hack, board, sizeof(board));
        snprintf(buf, len, "Locked terminal! %s\nReward: %s\n",
                 diffName[diff], reward[diff]);
        strncat(buf, board, len - strlen(buf) - 1);
        return;
    }

    // 60% combat encounter
    uint8_t tier = frpgGetTier(p.level);
    uint8_t pool[4];
    uint8_t poolSz = 0;
    for (uint8_t i = 0; i < FRPG_ENEMY_COUNT && poolSz < 4; i++) {
        if (FRPG_ENEMIES[i].tier == tier) pool[poolSz++] = i;
    }
    uint8_t eIdx = poolSz ? pool[random(poolSz)] : 0;
    uint16_t eHp  = (uint16_t)(FRPG_ENEMIES[eIdx].baseHp  + p.level * 5);
    uint8_t  eAtk = (uint8_t)(FRPG_ENEMIES[eIdx].baseAtk + p.level * 2);

    p.combat.active       = 1;
    p.combat.enemyIdx     = eIdx;
    p.combat.enemyType    = FRPG_ENEMIES[eIdx].etype;
    p.combat.enemyHp      = eHp;
    p.combat.baseEnemyAtk = eAtk;
    p.combat.limbEffects  = 0;
    p.combat.stunRounds   = 0;
    p.combat.vatsModeActive = 0;

    snprintf(buf, len,
        "A %s appears!\nEHP:%d ATK:%d | HP:%d/%d\n"
        "[A]ttack [D]efend\n"
        "[S]timpak [F]lee\n"
        "[V]ATS targeting (%d chg)",
        FRPG_ENEMIES[eIdx].name, eHp, eAtk, p.hp, p.maxHp, p.vatsCharges);
}

// ─── Settlement actions ───────────────────────────────────────────────────────

static void frpgShowMenu(FRPGPlayer &p, char *buf, size_t len)
{
    bool trainReady = (p.level < FRPG_MAX_LEVEL) && (p.xp >= FRPG_XP_THRESH[p.level]);
    bool bossReady  = (p.level == FRPG_MAX_LEVEL) && (p.xp >= FRPG_XP_THRESH[FRPG_MAX_LEVEL]);

    if (!p.alive) {
        uint32_t now = (uint32_t)getTime();
        uint32_t remain = (p.apResetTime > now) ? (p.apResetTime - now) : 0;
        snprintf(buf, len,
            "=== Wasteland RPG ===\n"
            "DEAD! Respawn in %uh%um\n"
            "[SH]op [ST]ats [LB]oard\n"
            "[X]Back to BBS",
            (unsigned)(remain/3600), (unsigned)((remain%3600)/60));
        return;
    }

    bool chengAvail = (p.level == FRPG_MAX_LEVEL && p.kills >= 1);
    const char *trainTag = (trainReady || bossReady) ? " READY!" : "";
    snprintf(buf, len,
        "=== Wastelad RPG ===\n"
        "[%s] Lvl:%d Caps:%d\n"
        "HP:%d/%d AP:%d/%d\n"
        "Wpn:%s Arm:%s\n"
        "Heal:%s Train:%s%s%s",
        p.name, p.level, p.caps,
        p.hp, p.maxHp, p.ap, FRPG_AP_MAX,
        FRPG_WEAPONS[p.weapon].name,
        FRPG_ARMORS[p.armor].name,
        p.healedToday ? "used" : "ready",
        trainTag[0] ? "READY!" : "not yet",
        chengAvail ? "\nCH-Cheng FINAL BOSS!" : "",
        "");
}

static void frpgDoStats(const FRPGPlayer &p, char *buf, size_t len)
{
    uint32_t xpNext = (p.level <= FRPG_MAX_LEVEL) ? FRPG_XP_THRESH[p.level] : 0;
    snprintf(buf, len,
        "[%s] Lvl:%d\n"
        "XP:%u/%u HP:%d/%d\n"
        "S:%d P:%d E:%d I:%d A:%d\n"
        "Wpn:%s\n"
        "Arm:%s\n"
        "Caps:%d Stim:%d VATS:%d/%d\n"
        "AP:%d/%d LM:%d Cheng:%d",
        p.name, p.level,
        (unsigned)p.xp, (unsigned)xpNext, p.hp, p.maxHp,
        p.str_, p.per, p.end_, p.intel, p.agi,
        FRPG_WEAPONS[p.weapon].name,
        FRPG_ARMORS[p.armor].name,
        p.caps, p.stimpaks, p.vatsCharges, p.vatsMax,
        p.ap, FRPG_AP_MAX, p.kills, p.chengKills);
}

static void frpgDoShop(const FRPGPlayer &p, char *buf, size_t len)
{
    // Show only items at or below player level
    int pos = snprintf(buf, len, "=== Wastelad Shop ===\nCaps:%d\n[W]eapons:\n", p.caps);
    for (int i = 0; i < FRPG_WEAPON_COUNT && (size_t)pos < len-40; i++) {
        if (FRPG_WEAPONS[i].levelReq <= p.level) {
            pos += snprintf(buf+pos, len-pos,
                "%d)%-14s %dc\n",
                i+1, FRPG_WEAPONS[i].name, FRPG_WEAPONS[i].cost);
        }
    }
    pos += snprintf(buf+pos, len-pos, "[A]rmor:\n");
    for (int i = 0; i < FRPG_ARMOR_COUNT && (size_t)pos < len-40; i++) {
        if (FRPG_ARMORS[i].levelReq <= p.level) {
            pos += snprintf(buf+pos, len-pos,
                "%d)%-12s %dc\n",
                i+1, FRPG_ARMORS[i].name, FRPG_ARMORS[i].cost);
        }
    }
    snprintf(buf+pos, len-pos, "Stimpak:%dc\nBUY W# / BUY A# / BUY S", FRPG_STIMPAK_COST);
}

static void frpgDoBuy(FRPGPlayer &p, const char *arg, char *buf, size_t len)
{
    if (!arg || arg[0] == '\0') { frpgDoShop(p, buf, len); return; }
    char type = toupper((unsigned char)arg[0]);
    int  num  = (arg[1]) ? atoi(arg+1) - 1 : -1; // 1-indexed to 0-indexed

    if (type == 'S') {
        // Stimpak
        if (p.stimpaks >= FRPG_STIMPAK_MAX) {
            snprintf(buf, len, "Stimpak bag full! (%d/%d)", p.stimpaks, FRPG_STIMPAK_MAX);
        } else if (p.caps < FRPG_STIMPAK_COST) {
            snprintf(buf, len, "Need %dc (have %d)", FRPG_STIMPAK_COST, p.caps);
        } else {
            p.caps -= FRPG_STIMPAK_COST;
            p.stimpaks++;
            snprintf(buf, len, "Bought Stimpak!\nStim:%d Caps:%d", p.stimpaks, p.caps);
        }
        return;
    }

    if (type == 'W' && num >= 0 && num < FRPG_WEAPON_COUNT) {
        const FRPGWeapon &w = FRPG_WEAPONS[num];
        if (p.level < w.levelReq) {
            snprintf(buf, len, "Need Lvl%d for %s", w.levelReq, w.name);
        } else if (p.caps < w.cost) {
            snprintf(buf, len, "Need %dc for %s (have %d)", w.cost, w.name, p.caps);
        } else {
            p.caps -= (uint16_t)w.cost;
            p.weapon = (uint8_t)num;
            snprintf(buf, len, "Equipped: %s\nCaps:%d", w.name, p.caps);
        }
        return;
    }

    if (type == 'A' && num >= 0 && num < FRPG_ARMOR_COUNT) {
        const FRPGArmor &a = FRPG_ARMORS[num];
        if (p.level < a.levelReq) {
            snprintf(buf, len, "Need Lvl%d for %s", a.levelReq, a.name);
        } else if (p.caps < a.cost) {
            snprintf(buf, len, "Need %dc for %s (have %d)", a.cost, a.name, p.caps);
        } else {
            p.caps -= (uint16_t)a.cost;
            p.armor = (uint8_t)num;
            snprintf(buf, len, "Equipped: %s\nCaps:%d", a.name, p.caps);
        }
        return;
    }

    snprintf(buf, len, "Use: BUY W# / BUY A# / BUY S");
}

static void frpgDoHeal(FRPGPlayer &p, char *buf, size_t len)
{
    if (p.healedToday) {
        snprintf(buf, len, "Doc says: come back\ntomorrow.\nHP:%d/%d", p.hp, p.maxHp);
        return;
    }
    p.hp = p.maxHp;
    p.healedToday = 1;
    snprintf(buf, len, "Doc patches you up!\nHP:%d/%d (free, once/day)", p.hp, p.maxHp);
}

static void frpgDoTrain(FRPGPlayer &p, char *buf, size_t len)
{
    if (p.combat.active) { frpgShowCombat(p, buf, len); return; }
    if (!p.alive) { snprintf(buf, len, "Can't train while dead!"); return; }

    bool trainReady = (p.level < FRPG_MAX_LEVEL) && (p.xp >= FRPG_XP_THRESH[p.level]);
    bool bossReady  = (p.level == FRPG_MAX_LEVEL) && (p.xp >= FRPG_XP_THRESH[FRPG_MAX_LEVEL]);

    if (!trainReady && !bossReady) {
        uint32_t needed = (p.level <= FRPG_MAX_LEVEL) ? FRPG_XP_THRESH[p.level] : 0;
        snprintf(buf, len, "Not ready.\nXP:%u/%u", (unsigned)p.xp, (unsigned)needed);
        return;
    }

    uint8_t tIdx = (uint8_t)(p.level - 1); // trainer index = level-1
    if (tIdx >= FRPG_TRAINER_COUNT) tIdx = FRPG_TRAINER_COUNT - 1;

    uint16_t tHp  = (uint16_t)(FRPG_TRAINERS[tIdx].hp + p.level * 5);
    uint8_t  tAtk = (uint8_t)(FRPG_TRAINERS[tIdx].atk + p.level);

    p.combat.active       = 2;
    p.combat.enemyIdx     = tIdx;
    p.combat.enemyType    = ETYPE_HUMANOID;
    p.combat.enemyHp      = tHp;
    p.combat.baseEnemyAtk = tAtk;
    p.combat.limbEffects  = 0;
    p.combat.stunRounds   = 0;
    p.combat.vatsModeActive = 0;
    p.combat.defending    = 0;

    snprintf(buf, len,
        "TRAINER: %s\nHP:%d ATK:%d\n%s\nA/D/S/F/V(%d)?",
        FRPG_TRAINERS[tIdx].name, tHp, tAtk,
        bossReady ? "FINAL BOSS FIGHT!" : "Defeat to level up!",
        p.vatsCharges);
}

static void frpgDoArena(FRPGPlayer &p, const char *arg, char *buf, size_t len)
{
    if (p.ap < 3) { snprintf(buf, len, "Arena costs 3 AP.\nAP:%d/%d", p.ap, FRPG_AP_MAX); return; }
    if (!p.alive) { snprintf(buf, len, "Can't fight while dead!"); return; }
    if (!arg || arg[0] == '\0') {
        snprintf(buf, len, "Usage: AR <name>\nCosts 3 AP. PvP!");
        return;
    }

    // Scan for target player by name
    char target[5] = {0};
    strncpy(target, arg, 4);
    target[4] = '\0';
    for (int i = 0; i < 4; i++) target[i] = toupper((unsigned char)target[i]);

    File dir = FSCom.open(FRPG_DIR, FILE_O_READ);
    FRPGPlayer tgt;
    bool found = false;
    if (dir) {
        BBS_FILE_VAR(f);
        while ((f = dir.openNextFile())) {
            if (f.isDirectory()) { f.close(); continue; }
            if (f.read((uint8_t *)&tgt, sizeof(tgt)) == sizeof(tgt)) {
                char tname[5] = {0};
                strncpy(tname, tgt.name, 4);
                for (int i = 0; i < 4; i++) tname[i] = toupper((unsigned char)tname[i]);
                if (tgt.nodeNum != p.nodeNum && strcmp(tname, target) == 0) {
                    found = true; f.close(); break;
                }
            }
            f.close();
        }
        dir.close();
    }

    if (!found) {
        snprintf(buf, len, "Player '%s' not found.\nCheck LB for names.", arg);
        return;
    }
    if (tgt.nodeNum == p.lastPvpTarget) {
        snprintf(buf, len, "Already attacked %s today.", tgt.name);
        return;
    }

    p.ap -= 3;
    p.lastPvpTarget = tgt.nodeNum;
    p.lastPvpTime   = (uint32_t)getTime();

    // Auto-resolve PvP: compare attack vs defense
    int myAtk  = FRPG_WEAPONS[p.weapon].damage + p.str_;
    int tgtDef = FRPG_ARMORS[tgt.armor].defense + tgt.end_;
    int myDef  = FRPG_ARMORS[p.armor].defense + p.end_;
    int tgtAtk = FRPG_WEAPONS[tgt.weapon].damage + tgt.str_;

    int myDmg  = myAtk  - tgtDef;  if (myDmg  < 1) myDmg  = 1;
    int tgtDmg = tgtAtk - myDef;   if (tgtDmg < 1) tgtDmg = 1;

    // Simulate ~5 rounds
    int myHp  = p.hp;
    int tgtHp = tgt.hp;
    for (int r = 0; r < 5; r++) {
        tgtHp -= myDmg;
        if (tgtHp <= 0) break;
        myHp  -= tgtDmg;
        if (myHp  <= 0) break;
    }

    if (tgtHp <= 0) {
        // Win
        uint16_t capGain = (uint16_t)(tgt.caps / 5);
        uint32_t xpGain  = 50 + tgt.level * 20;
        p.caps = (uint16_t)(p.caps + capGain);
        p.xp  += xpGain;
        snprintf(buf, len,
            "PvP WIN vs %s!\n+%dc +%uxp\nAP:%d/%d",
            tgt.name, capGain, (unsigned)xpGain, p.ap, FRPG_AP_MAX);
    } else {
        // Lose
        int dmgTaken = (int)(p.hp) - myHp;
        if (dmgTaken > 0) {
            p.hp = (myHp > 0) ? (uint16_t)myHp : 1;
        }
        snprintf(buf, len,
            "PvP LOSS vs %s!\n-%d HP\nHP:%d/%d AP:%d/%d",
            tgt.name, dmgTaken, p.hp, p.maxHp, p.ap, FRPG_AP_MAX);
    }
}

static void frpgDoBoard(char *buf, size_t len)
{
    FRPGPlayer top[5];
    uint32_t n = frpgTopPlayers(top, 5);
    int pos = snprintf(buf, len, "=== Wastelad Heroes ===\nC=Cheng B=BossLadder\n");
    for (uint32_t i = 0; i < n && (size_t)pos < len-30; i++) {
        pos += snprintf(buf+pos, len-pos,
            "%u. %-4s L%d C%d B%d\n",
            (unsigned)(i+1), top[i].name, top[i].level,
            top[i].chengKills, top[i].kills);
    }
    if (n == 0) snprintf(buf+pos, len-pos, "(No players yet)");
}

static void frpgDoAbout(char *buf, size_t len)
{
    snprintf(buf, len,
        "=== Wastelad RPG ===\n"
        "Based on the Wastelad holotape\n"
        "from Fallout 76. You escaped\n"
        "Vault 1 into a post-war world\n"
        "ruled by Chairman Cheng.\n"
        "Fight, level up, defeat bosses,\n"
        "and take down Cheng!\n"
        "Type HELP for commands.");
}

static void frpgDoHelp(const FRPGPlayer &p, char *buf, size_t len)
{
    if (p.combat.active) {
        snprintf(buf, len,
            "Combat Commands:\n"
            "A)ttack D)efend\n"
            "S)timpak F)lee\n"
            "V)ATS targeting\n"
            "(V costs 1 VATS charge)");
    } else {
        snprintf(buf, len,
            "=== Wastelad RPG ===\n"
            "EX-Explore  SH-Shop\n"
            "DR-Heal     TR-Train\n"
            "AR-Arena    TV-Tavern\n"
            "LB-Board    ST-Stats\n"
            "AB-About    CH-Cheng\n"
            "[X]Back to BBS");
    }
}

static void frpgDoTavern(char *buf, size_t len)
{
    static const char *quotes[] = {
        "Major Coot: \"The war didn't\nend. It just moved outside.\"",
        "Doc: \"I've seen worse.\nUsually right before worse.\"",
        "Maria Lou: \"Cheng's gold\nain't worth dyin' for. Is it?\"",
        "Mr. Pebbles: \"Meow.\"\n(He seems unimpressed.)",
        "Overseer Smith: \"Vault 1\nwas just the beginning.\"",
        "Old Barkeep: \"Nuka-Cola?\nAll I got is irradiated swamp\nwater. Same thing, really.\"",
        "Raider Drunk: \"Cheng took\nmy legs. Still walked here.\"",
        "Merchant: \"Caps? Sure.\nBut I don't trust 'em. Trade\nstimpaks instead.\"",
        "Major Coot: \"Every soldier\nwho fell out there — I know\ntheir names.\"",
        "Doc: \"Keep moving.\nBest medicine there is.\"",
    };
    uint8_t idx = (uint8_t)random(10);
    snprintf(buf, len, "=== The Rusty Nozzle ===\n%s", quotes[idx]);
}

static void frpgDoCheng(FRPGPlayer &p, char *buf, size_t len)
{
    if (!p.alive) {
        snprintf(buf, len, "You are dead!\nCan't face Cheng now.");
        return;
    }
    if (p.level < FRPG_MAX_LEVEL) {
        snprintf(buf, len, "You must reach Lvl%d\nbefore facing Cheng!\nLvl:%d", FRPG_MAX_LEVEL, p.level);
        return;
    }
    if (p.kills < 1) {
        snprintf(buf, len, "You must defeat the\nLagoon Monster first!\nUse TR when ready.");
        return;
    }
    if (p.combat.active) {
        frpgShowCombat(p, buf, len);
        return;
    }

    // Chairman Cheng: 999 HP + level*5, ATK 55 + level, HUMANOID
    uint16_t cHp  = (uint16_t)(999 + p.level * 5);
    uint8_t  cAtk = (uint8_t)(55 + p.level);

    p.combat.active       = 4;
    p.combat.enemyIdx     = 0;
    p.combat.enemyType    = ETYPE_HUMANOID;
    p.combat.enemyHp      = cHp;
    p.combat.baseEnemyAtk = cAtk;
    p.combat.limbEffects  = 0;
    p.combat.stunRounds   = 0;
    p.combat.vatsModeActive = 0;
    p.combat.defending    = 0;

    snprintf(buf, len,
        "CHAIRMAN CHENG!\n"
        "\"You escaped the Vault?\n"
        " Impressive. Die anyway.\"\n"
        "HP:%d ATK:%d\nA/D/S/V(%d)?",
        cHp, cAtk, p.vatsCharges);
}

// ─── Main dispatch ────────────────────────────────────────────────────────────

void frpgCommand(uint32_t nodeNum, const char *text, const char *shortName,
                 char *outBuf, size_t outLen, bool &exitGame)
{
    exitGame = false;
    outBuf[0] = '\0';

    FRPGPlayer p;
    bool existing = frpgLoadPlayer(nodeNum, p);
    if (!existing) {
        frpgNewPlayer(nodeNum, shortName, p);
        frpgSavePlayer(p);
        // Show a brief welcome then the main game menu (not ABOUT)
        char welcome[80];
        snprintf(welcome, sizeof(welcome),
                 "Welcome to the Wasteland, %s!\nType AB for lore. Good luck.", p.name);
        // Append welcome then menu in caller's buffer
        snprintf(outBuf, outLen, "%s", welcome);
        return;
    }
    // Empty text → show menu
    if (text[0] == '\0') {
        frpgShowMenu(p, outBuf, outLen);
        return;
    }

    frpgResetIfNeeded(p);

    // Trim leading whitespace
    while (*text && isspace((unsigned char)*text)) text++;

    // ── Route: VATS targeting mode ───────────────────────────────────────────
    if (p.combat.active && p.combat.vatsModeActive) {
        char c0 = toupper((unsigned char)text[0]);
        if (c0 == 'C' || strncasecmp(text, "CANCEL", 6) == 0) {
            p.combat.vatsModeActive = 0;
            frpgShowCombat(p, outBuf, outLen);
        } else if (c0 >= '1' && c0 <= '5') {
            frpgPickVATS(p, c0 - '1', outBuf, outLen);
        } else {
            frpgEnterVATS(p, outBuf, outLen); // re-show menu
        }
        frpgSavePlayer(p);
        return;
    }

    // ── Route: active combat ─────────────────────────────────────────────────
    if (p.combat.active) {
        char c0 = toupper((unsigned char)text[0]);
        switch (c0) {
            case 'A': frpgDoAttack(p, outBuf, outLen);  break;
            case 'D': frpgDoDefend(p, outBuf, outLen);  break;
            case 'S': frpgDoStimpak(p, outBuf, outLen); break;
            case 'F': frpgDoFlee(p, outBuf, outLen);    break;
            case 'V': frpgEnterVATS(p, outBuf, outLen); break;
            default:  frpgShowCombat(p, outBuf, outLen); break;
        }
        frpgSavePlayer(p);
        return;
    }

    // ── Route: active terminal hack ──────────────────────────────────────────
    if (p.hack.active) {
        if (strncasecmp(text, "SKIP", 4) == 0) {
            p.hack.active = 0;
            snprintf(outBuf, outLen, "Terminal abandoned.\nAP:%d/%d", p.ap, FRPG_AP_MAX);
        } else {
            // Check if it's a 5-letter word
            int tlen = 0;
            const char *t = text;
            while (t[tlen] && !isspace((unsigned char)t[tlen])) tlen++;
            if (tlen == 5) {
                frpgHackGuess(p, text, outBuf, outLen);
            } else {
                frpgShowHackBoard(p.hack, outBuf, outLen);
            }
        }
        frpgSavePlayer(p);
        return;
    }

    // ── Settlement command dispatch ──────────────────────────────────────────
    // Parse first word (up to 4 chars) uppercase, and rest as arg
    char cmd[5] = {0};
    int ci = 0;
    while (text[ci] && !isspace((unsigned char)text[ci]) && ci < 4) {
        cmd[ci] = toupper((unsigned char)text[ci]);
        ci++;
    }
    cmd[ci] = '\0';
    const char *arg = text[ci] ? text + ci + 1 : "";
    while (*arg && isspace((unsigned char)*arg)) arg++;

    // Match commands (check longest prefix first to avoid ambiguity)
    if (strcmp(cmd, "X") == 0 || strncasecmp(text, "BACK", 4) == 0) {
        exitGame = true;
        snprintf(outBuf, outLen, "73! Stay safe out there.");
    } else if (strcmp(cmd, "EX") == 0 || strcmp(cmd, "EXPL") == 0) {
        frpgStartExplore(p, outBuf, outLen);
    } else if (strcmp(cmd, "ST") == 0 || strcmp(cmd, "STAT") == 0) {
        frpgDoStats(p, outBuf, outLen);
    } else if (strcmp(cmd, "SH") == 0 || strcmp(cmd, "SHOP") == 0) {
        frpgDoShop(p, outBuf, outLen);
    } else if (strcmp(cmd, "BUY") == 0) {
        frpgDoBuy(p, arg, outBuf, outLen);
    } else if (strcmp(cmd, "DR") == 0 || strcmp(cmd, "HEAL") == 0) {
        frpgDoHeal(p, outBuf, outLen);
    } else if (strcmp(cmd, "TR") == 0 || strcmp(cmd, "TRAI") == 0) {
        frpgDoTrain(p, outBuf, outLen);
    } else if (strcmp(cmd, "AR") == 0 || strcmp(cmd, "AREN") == 0) {
        frpgDoArena(p, arg, outBuf, outLen);
    } else if (strcmp(cmd, "LB") == 0 || strcmp(cmd, "BOAR") == 0 || strcmp(cmd, "B") == 0) {
        frpgDoBoard(outBuf, outLen);
    } else if (strcmp(cmd, "AB") == 0 || strcmp(cmd, "ABOU") == 0) {
        frpgDoAbout(outBuf, outLen);
    } else if (strcmp(cmd, "H") == 0 || strcmp(cmd, "HELP") == 0 || strcmp(cmd, "?") == 0) {
        frpgDoHelp(p, outBuf, outLen);
    } else if (strcmp(cmd, "TV") == 0 || strcmp(cmd, "TAVE") == 0) {
        frpgDoTavern(outBuf, outLen);
    } else if (strcmp(cmd, "CH") == 0 || strcmp(cmd, "CHEN") == 0) {
        frpgDoCheng(p, outBuf, outLen);
    } else if (strcmp(cmd, "S") == 0 || strcmp(cmd, "STIM") == 0) {
        // Stimpak outside combat
        frpgDoStimpak(p, outBuf, outLen);
    } else if (strcmp(cmd, "RSET") == 0) {
        // Debug: force daily reset
        p.apResetTime = 0; // trigger reset on next call
        frpgResetIfNeeded(p); // fires immediately since apResetTime=0 < now
        snprintf(outBuf, outLen, "Daily reset!\nAP:%d/%d VATS:%d/%d\nHP:%d/%d",
                 p.ap, FRPG_AP_MAX, p.vatsCharges, p.vatsMax, p.hp, p.maxHp);
    } else {
        frpgShowMenu(p, outBuf, outLen);
    }

    frpgSavePlayer(p);
}


