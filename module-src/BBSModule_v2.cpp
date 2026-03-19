#include "BBSModule_v2.h"
#include "BBSWordle.h"
#include "BBSStorageLittleFS.h"
#include "BBSStoragePSRAM.h"
#include "Channels.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include <Arduino.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#ifdef ARCH_ESP32
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

BBSModule *bbsModule;

static const char *const BOARD_NAMES[BOARD_COUNT] = {"General", "Info", "News", "Urgent"};
static const char BOARD_KEYS[BOARD_COUNT] = {'g', 'i', 'n', 'u'};

// ─── Constructor / Destructor ─────────────────────────────────────────────

BBSModule::BBSModule() : SinglePortModule("bbs", meshtastic_PortNum_TEXT_MESSAGE_APP),
                         concurrency::OSThread("bbs_daily") {
    printf("[BBS] Constructor called\n");
    fflush(stdout);
    memset(sessions_, 0, sizeof(sessions_));

    bool initOk = false;

#ifdef ARCH_ESP32
    printf("[BBS] FreePSRAM=%u\n", (unsigned)ESP.getFreePsram());
    fflush(stdout);
    if (ESP.getFreePsram() > 1024 * 1024) {
        storage_ = new BBSStoragePSRAM();
        if (storage_) {
            initOk = storage_->init();
            if (!initOk) {
                printf("[BBS] PSRAM init failed, falling back to LittleFS\n");
                fflush(stdout);
                delete storage_;
                storage_ = nullptr;
            }
        }
    }
    if (!storage_) {
        printf("[BBS] Using LittleFS storage\n");
        fflush(stdout);
        storage_ = new BBSStorageLittleFS();
        if (storage_) initOk = storage_->init();
    }
#else
    storage_ = new BBSStorageLittleFS();
    if (storage_) initOk = storage_->init();
#endif

    printf("[BBS] Storage init %s\n", initOk ? "OK" : "FAILED");
    fflush(stdout);
}

void BBSModule::setup() {
    printf("[BBS] setup() called\n");
    fflush(stdout);
    wordleEnsureDir(); // ensure /bbs/wdl/ exists regardless of main storage backend
}

int32_t BBSModule::runOnce() {
    uint32_t t = getTime();
    // Wait until time is synced (must be after 2020-01-01)
    if (t < 1577836800UL) return 60000;

    uint32_t day = wordleDay();

    if (lastAnnouncedDay_ == 0) {
        // First valid time read — set baseline, seed forecast hour to skip current hour
        lastAnnouncedDay_ = day;
        lastForecastHour_ = (t % 86400) / 3600; // don't fire forecast on first sync
        printf("[BBS] runOnce: time synced, wordle day=%u\n", day);
        fflush(stdout);
        return 60000;
    }

    if (day > lastAnnouncedDay_) {
        // New wordle day - announce and show yesterday's standings
        char msg[200] = {0};
        char standings[160] = {0};
        buildStandings(lastAnnouncedDay_, standings, sizeof(standings));

        if (strncmp(standings, "No Wordle", 9) == 0) {
            snprintf(msg, sizeof(msg), "New Wordle word ready!\nDM [W] to play.");
        } else {
            // Yesterday's standings + new word notice
            snprintf(msg, sizeof(msg), "New Wordle word!\n%s\nDM [W] to play.", standings);
        }
        sendToPublicChannel(msg);

        wordlePruneOldDays(day);
        lastAnnouncedDay_ = day;
        printf("[BBS] runOnce: day transition to %u, announced.\n", day);
        fflush(stdout);
    }

    // Check for 6am (UTC hour 6) or 6pm (UTC hour 18) forecast broadcast
    uint8_t hour = (t % 86400) / 3600;
    if ((hour == 6 || hour == 18) && hour != lastForecastHour_) {
        lastForecastHour_ = hour;
        char fcBuf[200] = {0};
        fetchForecast(fcBuf, sizeof(fcBuf));
        if (fcBuf[0] != '\0') {
            sendToPublicChannel(fcBuf);
            printf("[BBS] runOnce: sent forecast for UTC hour %u\n", hour);
        }
    } else if (hour != 6 && hour != 18) {
        lastForecastHour_ = 255; // reset so next 6am/6pm fires
    }

    return 60000; // check every 60 seconds
}

BBSModule::~BBSModule() {
    delete storage_;
    storage_ = nullptr;
}

const char *BBSModule::boardName(uint8_t board) {
    if (board < BOARD_COUNT) return BOARD_NAMES[board];
    return "General";
}

// ─── Packet handling ──────────────────────────────────────────────────────

bool BBSModule::wantPacket(const meshtastic_MeshPacket *p) {
    return SinglePortModule::wantPacket(p);
}

// ─── Seen-users file (/bbs/seen.bin) — sequence of uint32_t node numbers ────
// Written once per node; gates the one-time welcome message.

static const char *SEEN_FILE = "/bbs/seen.bin";

static bool seenUserCheck(uint32_t nodeNum) {
    File f = FSCom.open(SEEN_FILE, FILE_O_READ);
    if (!f) return false;
    uint32_t n;
    while (f.available() >= (int)sizeof(uint32_t)) {
        f.read((uint8_t *)&n, sizeof(uint32_t));
        if (n == nodeNum) { f.close(); return true; }
    }
    f.close();
    return false;
}

static void seenUserAdd(uint32_t nodeNum) {
    File f = FSCom.open(SEEN_FILE, "a");
    if (!f) return;
    f.write((const uint8_t *)&nodeNum, sizeof(uint32_t));
    f.close();
}

ProcessMessage BBSModule::handleReceived(const meshtastic_MeshPacket &mp) {
    if (!storage_) return ProcessMessage::CONTINUE;

    char buf[260];
    memset(buf, 0, sizeof(buf));
    size_t n = mp.decoded.payload.size;
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);

    // Trim trailing whitespace
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }

    printf("[BBS] Rx from=0x%08x to=0x%08x: %s\n", mp.from, mp.to, buf);
    fflush(stdout);

    bool isDM = (mp.to == nodeDB->getNodeNum());

    if (isDM) {
        const char *text = buf;
        if (strncasecmp(text, "!bbs", 4) == 0) {
            text += 4;
            while (*text == ' ' || *text == '\t') text++;
        }
        return handleDM(mp, text);
    } else {
        if (strncasecmp(buf, "!bbs", 4) != 0) return ProcessMessage::CONTINUE;
        const char *cmd = buf + 4;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        return handleChannelCmd(mp, cmd);
    }
}

// ─── Session management ───────────────────────────────────────────────────

BBSSession *BBSModule::getOrCreateSession(uint32_t nodeNum) {
    BBSSession *oldest = nullptr;
    for (int i = 0; i < BBS_MAX_SESSIONS; i++) {
        if (sessions_[i].nodeNum == nodeNum) return &sessions_[i];
        if (!oldest || sessions_[i].lastActivity < oldest->lastActivity) oldest = &sessions_[i];
    }
    memset(oldest, 0, sizeof(BBSSession));
    oldest->nodeNum = nodeNum;
    oldest->state = BBS_STATE_IDLE;
    oldest->bulletinPage = 1;
    oldest->qslPage = 1;
    oldest->currentBoard = BOARD_GENERAL;
    return oldest;
}

void BBSModule::expireSessions(uint32_t now) {
    if (now == 0) return; // no time sync yet, don't expire anything
    for (int i = 0; i < BBS_MAX_SESSIONS; i++) {
        if (sessions_[i].nodeNum != 0 &&
            sessions_[i].lastActivity != 0 &&
            (now - sessions_[i].lastActivity) > BBS_SESSION_TIMEOUT_S) {
            printf("[BBS] Session expired for 0x%08x\n", sessions_[i].nodeNum);
            memset(&sessions_[i], 0, sizeof(BBSSession));
        }
    }
}

// ─── DM handler ───────────────────────────────────────────────────────────

ProcessMessage BBSModule::handleDM(const meshtastic_MeshPacket &mp, const char *text) {
    uint32_t now = getTime();
    expireSessions(now);
    BBSSession *session = getOrCreateSession(mp.from);
    if (!session) return ProcessMessage::CONTINUE;
    session->lastActivity = now;
    return dispatchState(mp, *session, text);
}

ProcessMessage BBSModule::dispatchState(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    switch (session.state) {
        case BBS_STATE_IDLE: {
            session.state = BBS_STATE_MAIN;
            bool firstTime = !seenUserCheck(mp.from);
            if (firstTime) {
                seenUserAdd(mp.from);
                sendReply(mp, "Welcome to TinyBBS!\nSend [H] for help.");
            }
            // Always process the text as a main-menu command (or show menu if empty)
            return handleStateMain(mp, session, text);
        }
        case BBS_STATE_MAIN:           return handleStateMain(mp, session, text);
        case BBS_STATE_BULLETIN:       return handleStateBulletin(mp, session, text);
        case BBS_STATE_BULLETIN_BOARD: return handleStateBulletinBoard(mp, session, text);
        case BBS_STATE_MAIL:           return handleStateMail(mp, session, text);
        case BBS_STATE_MAIL_SEND_TO:   return handleStateMailSendTo(mp, session, text);
        case BBS_STATE_MAIL_SEND_SUBJECT: return handleStateMailSendSubject(mp, session, text);
        case BBS_STATE_MAIL_SEND_BODY: return handleStateMailSendBody(mp, session, text);
        case BBS_STATE_QSL:            return handleStateQSL(mp, session, text);
        case BBS_STATE_WORDLE:         return handleStateWordle(mp, session, text);
        default:
            session.state = BBS_STATE_MAIN;
            sendMainMenu(mp, session);
            return ProcessMessage::STOP;
    }
}

// ─── Menu display ─────────────────────────────────────────────────────────

void BBSModule::sendMainMenu(const meshtastic_MeshPacket &req, BBSSession &session) {
    uint32_t unread = storage_->countUnreadMail(req.from);
    char menu[200];
    if (unread > 0) {
        snprintf(menu, sizeof(menu),
                 "=== TinyBBS ===\n"
                 "[B]ulletins\n"
                 "[M]ail (%u unread)\n"
                 "[Q]SL\n"
                 "[W]ordle\n"
                 "[F]orecast\n"
                 "[S]tats\n"
                 "[X]Exit",
                 unread);
    } else {
        snprintf(menu, sizeof(menu),
                 "=== TinyBBS ===\n"
                 "[B]ulletins\n"
                 "[M]ail\n"
                 "[Q]SL\n"
                 "[W]ordle\n"
                 "[F]orecast\n"
                 "[S]tats\n"
                 "[X]Exit");
    }
    sendReply(req, menu);
}

void BBSModule::sendBoardSelectMenu(const meshtastic_MeshPacket &req) {
    uint32_t counts[BOARD_COUNT];
    for (int i = 0; i < BOARD_COUNT; i++) {
        counts[i] = storage_->totalActiveBulletins((BBSBoard)i);
    }
    char menu[200];
    snprintf(menu, sizeof(menu),
             "=== Bulletins ===\n"
             "[G]eneral (%u)\n"
             "[I]nfo (%u)\n"
             "[N]ews (%u)\n"
             "[U]rgent (%u)\n"
             "[X]Back",
             counts[BOARD_GENERAL], counts[BOARD_INFO],
             counts[BOARD_NEWS], counts[BOARD_URGENT]);
    sendReply(req, menu);
}

void BBSModule::sendBulletinMenu(const meshtastic_MeshPacket &req, uint8_t board) {
    uint32_t total = storage_->totalActiveBulletins(board);
    char menu[200];
    snprintf(menu, sizeof(menu),
             "=== %s (%u) ===\n"
             "[L]ist\n"
             "[P]ost <text>\n"
             "[R]ead <#>\n"
             "[D]elete <#>\n"
             "[X]Back",
             boardName(board), total);
    sendReply(req, menu);
}

void BBSModule::sendMailMenu(const meshtastic_MeshPacket &req, uint32_t nodeNum) {
    uint32_t unread = storage_->countUnreadMail(nodeNum);
    char menu[200];
    snprintf(menu, sizeof(menu),
             "=== Mail (%u unread) ===\n"
             "[L]ist\n"
             "[R]ead <#>\n"
             "[S]end\n"
             "[D]elete <#>\n"
             "[X]Back",
             unread);
    sendReply(req, menu);
}

void BBSModule::sendQSLMenu(const meshtastic_MeshPacket &req) {
    uint32_t total = storage_->totalActiveQSL();
    char menu[200];
    snprintf(menu, sizeof(menu),
             "=== QSL Board (%u) ===\n"
             "[L]ist\n"
             "[P]ost mine\n"
             "[X]Back",
             total);
    sendReply(req, menu);
}

// ─── State handlers ───────────────────────────────────────────────────────

ProcessMessage BBSModule::handleStateMain(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendMainMenu(mp, session); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);
    switch (cmd) {
        case 'b':
            session.state = BBS_STATE_BULLETIN;
            sendBoardSelectMenu(mp);
            break;
        case 'm':
            session.state = BBS_STATE_MAIL;
            sendMailMenu(mp, mp.from);
            break;
        case 'q':
            session.state = BBS_STATE_QSL;
            session.qslPage = 1;
            sendQSLMenu(mp);
            break;
        case 'w':
            doWordleStart(mp, session);
            break;
        case 'f':
            doForecast(mp);
            break;
        case 's':
            doStats(mp);
            break;
        case 'x':
            session.state = BBS_STATE_IDLE;
            sendReply(mp, "73 de TinyBBS - bye!");
            break;
        case '?': case 'h':
            sendReply(mp,
                      "TinyBBS Help:\n"
                      "DM this node to use BBS\n"
                      "[B]ulletins\n"
                      "[M]ail\n"
                      "[Q]SL\n"
                      "[W]ordle\n"
                      "[F]orecast\n"
                      "[S]tats\n"
                      "[X]Exit\n"
                      "Channel: !bbs <cmd>");
            break;
        default:
            sendMainMenu(mp, session);
            break;
    }
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateBulletin(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    // Board selection state
    if (!text || text[0] == '\0') { sendBoardSelectMenu(mp); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);
    if (cmd == 'x') {
        session.state = BBS_STATE_MAIN;
        sendMainMenu(mp, session);
        return ProcessMessage::STOP;
    }
    // Match board key
    for (int i = 0; i < BOARD_COUNT; i++) {
        if (cmd == BOARD_KEYS[i]) {
            session.currentBoard = (uint8_t)i;
            session.bulletinPage = 1;
            session.state = BBS_STATE_BULLETIN_BOARD;
            sendBulletinMenu(mp, session.currentBoard);
            return ProcessMessage::STOP;
        }
    }
    sendBoardSelectMenu(mp);
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateBulletinBoard(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendBulletinMenu(mp, session.currentBoard); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);
    const char *arg = text + 1;
    while (*arg == ' ' || *arg == '\t') arg++;

    switch (cmd) {
        case 'l':
            session.bulletinPage = 1;
            doBulletinList(mp, session.bulletinPage, session.currentBoard);
            break;
        case 'n':
            session.bulletinPage++;
            doBulletinList(mp, session.bulletinPage, session.currentBoard);
            break;
        case 'r': {
            uint32_t id = *arg ? (uint32_t)atoi(arg) : 0;
            if (id == 0) sendReply(mp, "Usage: R <id#>");
            else doBulletinRead(mp, id);
            break;
        }
        case 'p':
            if (*arg == '\0') sendReply(mp, "Usage: P <message text>");
            else doBulletinPost(mp, arg, session.currentBoard);
            break;
        case 'd': {
            uint32_t id = *arg ? (uint32_t)atoi(arg) : 0;
            if (id == 0) sendReply(mp, "Usage: D <id#>");
            else doBulletinDelete(mp, id);
            break;
        }
        case 'x':
            session.state = BBS_STATE_BULLETIN;
            sendBoardSelectMenu(mp);
            break;
        default:
            sendBulletinMenu(mp, session.currentBoard);
            break;
    }
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateMail(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendMailMenu(mp, mp.from); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);
    const char *arg = text + 1;
    while (*arg == ' ' || *arg == '\t') arg++;

    switch (cmd) {
        case 'l': doMailList(mp, mp.from); break;
        case 'r': {
            uint32_t id = *arg ? (uint32_t)atoi(arg) : 0;
            if (id == 0) sendReply(mp, "Usage: R <id#>");
            else doMailRead(mp, id);
            break;
        }
        case 's':
            session.state = BBS_STATE_MAIL_SEND_TO;
            memset(session.mailSendTo, 0, sizeof(session.mailSendTo));
            memset(session.mailSendSubject, 0, sizeof(session.mailSendSubject));
            sendReply(mp, "Send to (short name or !nodenum):");
            break;
        case 'd': {
            uint32_t id = *arg ? (uint32_t)atoi(arg) : 0;
            if (id == 0) sendReply(mp, "Usage: D <id#>");
            else doMailDelete(mp, id);
            break;
        }
        case 'x':
            session.state = BBS_STATE_MAIN;
            sendMainMenu(mp, session);
            break;
        default:
            sendMailMenu(mp, mp.from);
            break;
    }
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateMailSendTo(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendReply(mp, "Send to:"); return ProcessMessage::STOP; }
    if ((text[0] == 'x' || text[0] == 'X') && text[1] == '\0') {
        session.state = BBS_STATE_MAIL; sendMailMenu(mp, mp.from); return ProcessMessage::STOP;
    }
    strncpy(session.mailSendTo, text, sizeof(session.mailSendTo) - 1);
    session.mailSendTo[sizeof(session.mailSendTo) - 1] = '\0';
    session.state = BBS_STATE_MAIL_SEND_SUBJECT;
    char prompt[80];
    snprintf(prompt, sizeof(prompt), "Subject (to %s):", session.mailSendTo);
    sendReply(mp, prompt);
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateMailSendSubject(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendReply(mp, "Enter subject:"); return ProcessMessage::STOP; }
    if ((text[0] == 'x' || text[0] == 'X') && text[1] == '\0') {
        session.state = BBS_STATE_MAIL; sendReply(mp, "Cancelled."); sendMailMenu(mp, mp.from); return ProcessMessage::STOP;
    }
    strncpy(session.mailSendSubject, text, sizeof(session.mailSendSubject) - 1);
    session.mailSendSubject[sizeof(session.mailSendSubject) - 1] = '\0';
    session.state = BBS_STATE_MAIL_SEND_BODY;
    char prompt[80];
    snprintf(prompt, sizeof(prompt), "Message body\n(or X to cancel):");
    sendReply(mp, prompt);
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateMailSendBody(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendReply(mp, "Enter message:"); return ProcessMessage::STOP; }
    if ((text[0] == 'x' || text[0] == 'X') && text[1] == '\0') {
        session.state = BBS_STATE_MAIL; sendReply(mp, "Cancelled."); sendMailMenu(mp, mp.from); return ProcessMessage::STOP;
    }
    doMailSend(mp, session.mailSendTo, session.mailSendSubject, text);
    session.state = BBS_STATE_MAIL;
    sendMailMenu(mp, mp.from);
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateQSL(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendQSLMenu(mp); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);
    switch (cmd) {
        case 'l': session.qslPage = 1; doQSLList(mp, session.qslPage); break;
        case 'n': session.qslPage++;   doQSLList(mp, session.qslPage); break;
        case 'p': doQSLPost(mp); break;
        case 'x': session.state = BBS_STATE_MAIN; sendMainMenu(mp, session); break;
        default:  sendQSLMenu(mp); break;
    }
    return ProcessMessage::STOP;
}

// ─── Wordle ───────────────────────────────────────────────────────────────

// ─── Wordle FS helpers (always LittleFS, persistent across reboots) ───────────
// Score file: /bbs/wdl/d<day>.bin — sequence of BBSWordleScore structs

static const char *WORDLE_DIR = "/bbs/wdl";

uint32_t BBSModule::wordleDay() {
    uint32_t t = getTime();
    if (t < 7 * 3600) return 0;
    return (t - 7 * 3600) / 86400; // UTC 7am boundary
}

void BBSModule::wordleEnsureDir() {
    if (!FSCom.exists(WORDLE_DIR)) FSCom.mkdir(WORDLE_DIR);
}

static void wordleScorePath(uint32_t day, char *path, size_t len) {
    snprintf(path, len, "/bbs/wdl/d%" PRIu32 ".bin", day);
}

uint32_t BBSModule::wordleLoadScores(uint32_t day, BBSWordleScore *scores, uint32_t max) {
    if (!scores || max == 0) return 0;
    char path[48]; wordleScorePath(day, path, sizeof(path));
    File f = FSCom.open(path, FILE_O_READ);
    if (!f) return 0;
    uint32_t count = 0;
    while (count < max && f.available() >= (int)sizeof(BBSWordleScore)) {
        f.read((uint8_t *)&scores[count], sizeof(BBSWordleScore));
        count++;
    }
    f.close();
    return count;
}

bool BBSModule::wordleHasPlayed(uint32_t day, uint32_t nodeNum) {
    BBSWordleScore scores[BBS_WORDLE_MAX_SCORES];
    uint32_t count = wordleLoadScores(day, scores, BBS_WORDLE_MAX_SCORES);
    for (uint32_t i = 0; i < count; i++) {
        if (scores[i].nodeNum == nodeNum) return true;
    }
    return false;
}

bool BBSModule::wordleSaveScore(uint32_t day, const BBSWordleScore &score) {
    wordleEnsureDir();
    BBSWordleScore existing[BBS_WORDLE_MAX_SCORES];
    uint32_t count = wordleLoadScores(day, existing, BBS_WORDLE_MAX_SCORES);
    // Reject duplicate
    for (uint32_t i = 0; i < count; i++) {
        if (existing[i].nodeNum == score.nodeNum) return false;
    }
    if (count >= BBS_WORDLE_MAX_SCORES) return false;
    existing[count++] = score;
    char path[48]; wordleScorePath(day, path, sizeof(path));
    File f = FSCom.open(path, FILE_O_WRITE);
    if (!f) return false;
    for (uint32_t i = 0; i < count; i++) {
        f.write((const uint8_t *)&existing[i], sizeof(BBSWordleScore));
    }
    f.close();
    return true;
}

void BBSModule::wordlePruneOldDays(uint32_t currentDay) {
    if (currentDay < 2) return;
    char path[48]; wordleScorePath(currentDay - 2, path, sizeof(path));
    if (FSCom.exists(path)) FSCom.remove(path);
}

void BBSModule::buildStandings(uint32_t day, char *buf, size_t bufLen) {
    BBSWordleScore scores[BBS_WORDLE_MAX_SCORES];
    uint32_t count = wordleLoadScores(day, scores, BBS_WORDLE_MAX_SCORES);

    if (count == 0) {
        snprintf(buf, bufLen, "No Wordle scores today.");
        return;
    }

    // Sort by guesses ascending (0=DNF treated as 7 for sorting)
    for (uint32_t i = 0; i < count - 1; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            uint8_t a = scores[i].guesses == 0 ? 7 : scores[i].guesses;
            uint8_t b = scores[j].guesses == 0 ? 7 : scores[j].guesses;
            if (a > b) { BBSWordleScore tmp = scores[i]; scores[i] = scores[j]; scores[j] = tmp; }
        }
    }

    size_t pos = snprintf(buf, bufLen, "Wordle scores:\n");
    for (uint32_t i = 0; i < count && pos + 24 < bufLen; i++) {
        char line[24];
        if (scores[i].guesses == 0)
            snprintf(line, sizeof(line), "%s: X/6\n", scores[i].shortName);
        else
            snprintf(line, sizeof(line), "%s: %u/6\n", scores[i].shortName, scores[i].guesses);
        size_t lineLen = strlen(line);
        if (pos + lineLen >= bufLen) break;
        memcpy(buf + pos, line, lineLen);
        pos += lineLen;
        buf[pos] = '\0';
    }
}

void BBSModule::sendToPublicChannel(const char *text) {
    if (!text) return;
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = channels.getPrimaryIndex();
    p->want_ack = false;
    p->decoded.want_response = false;
    size_t len = strlen(text);
    if (len > REPLY_MAX_LEN) len = REPLY_MAX_LEN;
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text, len);
    service->sendToMesh(p);
}

void BBSModule::doWordleStart(const meshtastic_MeshPacket &req, BBSSession &session) {
    uint32_t day = wordleDay();

    // Check if already played today (persistent LittleFS check — survives reboots)
    if (wordleHasPlayed(day, req.from)) {
        char standings[180];
        buildStandings(day, standings, sizeof(standings));
        char msg[200];
        snprintf(msg, sizeof(msg), "Already played today!\n%s", standings);
        sendReply(req, msg);
        return; // stay in BBS_STATE_MAIN
    }

    // Pick today's daily word (same for everyone)
    const char *word = wordlePickWord(day);
    strncpy(session.wordleTarget, word, 5);
    session.wordleTarget[5] = '\0';
    session.wordleGuesses = 0;
    session.wordleDay = day;
    session.state = BBS_STATE_WORDLE;

    sendReply(req,
              "--- Wordle ---\n"
              "Guess a 5-letter word.\n"
              "UPPER=right place\n"
              "lower=wrong place\n"
              "_=not in word\n"
              "6 tries. X to quit.\n"
              "Guess 1/6:");
}

ProcessMessage BBSModule::handleStateWordle(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') {
        char prompt[40];
        snprintf(prompt, sizeof(prompt), "Guess %u/6:", session.wordleGuesses + 1);
        sendReply(mp, prompt);
        return ProcessMessage::STOP;
    }

    // Allow quit
    if ((text[0] == 'x' || text[0] == 'X') && text[1] == '\0') {
        char msg[60];
        snprintf(msg, sizeof(msg), "Quit. Word was: %s", session.wordleTarget);
        sendReply(mp, msg);
        session.state = BBS_STATE_MAIN;
        sendMainMenu(mp, session);
        return ProcessMessage::STOP;
    }

    // Validate: must be exactly 5 alpha chars
    char guess[6] = {0};
    int guessLen = 0;
    for (int i = 0; text[i] && guessLen < 6; i++) {
        if (isalpha((unsigned char)text[i])) {
            guess[guessLen++] = tolower((unsigned char)text[i]);
        }
    }
    if (guessLen != 5) {
        sendReply(mp, "Enter a 5-letter word.");
        return ProcessMessage::STOP;
    }

    session.wordleGuesses++;

    // Compute feedback
    char fb[5];
    wordleFeedback(guess, session.wordleTarget, fb);

    // Check win
    bool won = (fb[0]=='G' && fb[1]=='G' && fb[2]=='G' && fb[3]=='G' && fb[4]=='G');

    // Build feedback: UPPERCASE = right place, lowercase = wrong place, · = not in word
    char reply[120];
    char upper[6];
    char fbStr[16] = {0}; // feedback string (middle dot is 2-byte UTF-8)
    size_t fbPos = 0;
    for (int i = 0; i < 5; i++) {
        upper[i] = toupper((unsigned char)guess[i]);
        if (fb[i] == 'G') {
            fbStr[fbPos++] = upper[i];
        } else if (fb[i] == 'Y') {
            fbStr[fbPos++] = guess[i];
        } else {
            fbStr[fbPos++] = '\xC2'; // UTF-8 middle dot U+00B7
            fbStr[fbPos++] = '\xB7';
        }
    }
    fbStr[fbPos] = '\0';
    upper[5] = '\0';

    auto saveAndShowStandings = [&](uint8_t guessCount) {
        // Save score to LittleFS (persistent across reboots)
        BBSWordleScore score;
        score.nodeNum = mp.from;
        score.guesses = guessCount;
        const char *name = getNodeShortName(mp.from);
        if (name) strncpy(score.shortName, name, BBS_SHORT_NAME_LEN - 1);
        else snprintf(score.shortName, BBS_SHORT_NAME_LEN, "!%08x", mp.from);
        score.shortName[BBS_SHORT_NAME_LEN - 1] = '\0';
        wordleSaveScore(session.wordleDay, score);
        // Show standings
        char standings[180];
        buildStandings(session.wordleDay, standings, sizeof(standings));
        delay(MULTIPART_DELAY_MS);
        sendReply(mp, standings);
    };

    if (won) {
        snprintf(reply, sizeof(reply),
                 "%c%c%c%c%c - Got it in %u!",
                 upper[0], upper[1], upper[2], upper[3], upper[4],
                 session.wordleGuesses);
        sendReply(mp, reply);
        session.wordleGamesPlayed++;
        session.state = BBS_STATE_MAIN;
        saveAndShowStandings(session.wordleGuesses);
        delay(MULTIPART_DELAY_MS);
        sendMainMenu(mp, session);
    } else if (session.wordleGuesses >= 6) {
        snprintf(reply, sizeof(reply),
                 "%s - Game over! Word: %s",
                 fbStr, session.wordleTarget);
        sendReply(mp, reply);
        session.wordleGamesPlayed++;
        session.state = BBS_STATE_MAIN;
        saveAndShowStandings(0); // 0 = DNF
        delay(MULTIPART_DELAY_MS);
        sendMainMenu(mp, session);
    } else {
        snprintf(reply, sizeof(reply),
                 "%s  Guess %u/6:",
                 fbStr, session.wordleGuesses + 1);
        sendReply(mp, reply);
    }

    return ProcessMessage::STOP;
}

// ─── Channel one-shot handler ─────────────────────────────────────────────

ProcessMessage BBSModule::handleChannelCmd(const meshtastic_MeshPacket &mp, const char *cmd) {
    if (!cmd || *cmd == '\0' || *cmd == '?') {
        sendReply(mp,
                  "TinyBBS: DM this node for full menu\n"
                  "!bbs qsl, !bbs post <txt>\n"
                  "!bbs list, !bbs stats\n"
                  "!bbs wx");
        return ProcessMessage::STOP;
    }
    if (strncasecmp(cmd, "qsl", 3) == 0) {
        doQSLPost(mp);
    } else if (strncasecmp(cmd, "post", 4) == 0) {
        const char *text = cmd + 4;
        while (*text == ' ' || *text == '\t') text++;
        doBulletinPost(mp, text, BOARD_GENERAL);
    } else if (strncasecmp(cmd, "list", 4) == 0) {
        doBulletinList(mp, 1, BOARD_ALL);
    } else if (strncasecmp(cmd, "stats", 5) == 0) {
        doStats(mp);
    } else if (strncasecmp(cmd, "wx", 2) == 0 || strncasecmp(cmd, "forecast", 8) == 0) {
        doForecast(mp);
    } else {
        sendReply(mp, "Cmds: !bbs qsl, !bbs post <txt>, !bbs list, !bbs stats, !bbs wx");
    }
    return ProcessMessage::STOP;
}

// ─── Action implementations ───────────────────────────────────────────────

void BBSModule::doBulletinList(const meshtastic_MeshPacket &req, uint32_t pageNum, uint8_t board) {
    uint32_t offset = (pageNum - 1) * PAGE_SIZE;
    BBSBulletinHeader headers[PAGE_SIZE];
    uint32_t count = storage_->listBulletins(headers, PAGE_SIZE, offset, board);

    if (count == 0) {
        sendReply(req, pageNum == 1 ? "No bulletins here.\nSend P <text> to post!" : "No more bulletins.");
        return;
    }

    char reply[512] = {0};
    if (board == BOARD_ALL) {
        snprintf(reply, sizeof(reply), "Bulletins(pg%u):\n", pageNum);
    } else {
        snprintf(reply, sizeof(reply), "%s(pg%u):\n", boardName(board), pageNum);
    }
    for (uint32_t i = 0; i < count; i++) {
        char line[100];
        if (board == BOARD_ALL) {
            snprintf(line, sizeof(line), "#%u [%c]%s: %.30s\n",
                     headers[i].id,
                     (headers[i].board < BOARD_COUNT) ? BOARD_NAMES[headers[i].board][0] : 'G',
                     headers[i].authorName, headers[i].preview);
        } else {
            snprintf(line, sizeof(line), "#%u %s: %.35s\n",
                     headers[i].id, headers[i].authorName, headers[i].preview);
        }
        strncat(reply, line, sizeof(reply) - strlen(reply) - 1);
    }
    strncat(reply, "[N]ext [R]# [P]ost", sizeof(reply) - strlen(reply) - 1);
    sendReplyMultipart(req, reply);
}

void BBSModule::doBulletinRead(const meshtastic_MeshPacket &req, uint32_t id) {
    BBSBulletin bulletin;
    if (!storage_->loadBulletin(id, bulletin)) { sendReply(req, "Bulletin not found."); return; }
    char reply[512];
    snprintf(reply, sizeof(reply), "#%u [%s] from %s:\n%s", id, boardName(bulletin.board), bulletin.authorName, bulletin.body);
    sendReplyMultipart(req, reply);
}

void BBSModule::doBulletinPost(const meshtastic_MeshPacket &req, const char *text, uint8_t board) {
    if (!text || text[0] == '\0') { sendReply(req, "Usage: P <message text>"); return; }
    if (strlen(text) > BBS_MSG_MAX_LEN) { sendReply(req, "Too long (200 char max)."); return; }

    BBSBulletin bulletin;
    bulletin.id = storage_->nextBulletinId();
    bulletin.authorNode = req.from;
    bulletin.timestamp = getTime();
    bulletin.active = true;
    bulletin.board = board;

    const char *name = getNodeShortName(req.from);
    if (name) strncpy(bulletin.authorName, name, BBS_SHORT_NAME_LEN - 1);
    else snprintf(bulletin.authorName, BBS_SHORT_NAME_LEN, "!%08x", req.from);
    bulletin.authorName[BBS_SHORT_NAME_LEN - 1] = '\0';

    strncpy(bulletin.body, text, BBS_MSG_MAX_LEN);
    bulletin.body[BBS_MSG_MAX_LEN] = '\0';

    if (storage_->storeBulletin(bulletin)) {
        char reply[80];
        snprintf(reply, sizeof(reply), "Posted #%u to %s!", bulletin.id, boardName(board));
        sendReply(req, reply);
    } else {
        sendReply(req, "Failed to post bulletin.");
    }
}

void BBSModule::doBulletinDelete(const meshtastic_MeshPacket &req, uint32_t id) {
    if (storage_->deleteBulletin(id, req.from)) sendReply(req, "Deleted.");
    else sendReply(req, "Not found or not yours.");
}

void BBSModule::doMailList(const meshtastic_MeshPacket &req, uint32_t recipientNode) {
    BBSMailHeader headers[PAGE_SIZE];
    uint32_t count = storage_->listMail(recipientNode, headers, PAGE_SIZE, 0);

    if (count == 0) { sendReply(req, "No mail.\nSend S to compose."); return; }

    char reply[512] = {0};
    snprintf(reply, sizeof(reply), "Mail:\n");
    for (uint32_t i = 0; i < count; i++) {
        char line[100];
        const char *subj = headers[i].subject[0] ? headers[i].subject : "(no subject)";
        snprintf(line, sizeof(line), "#%u %s%s: %s\n",
                 headers[i].id, headers[i].read ? "" : "[NEW]",
                 headers[i].fromName, subj);
        strncat(reply, line, sizeof(reply) - strlen(reply) - 1);
    }
    strncat(reply, "[R]# read [D]# del", sizeof(reply) - strlen(reply) - 1);
    sendReplyMultipart(req, reply);
}

void BBSModule::doMailRead(const meshtastic_MeshPacket &req, uint32_t id) {
    BBSMailMsg msg;
    if (!storage_->loadMail(id, req.from, msg)) { sendReply(req, "Message not found."); return; }
    storage_->markMailRead(id, req.from);
    char reply[512];
    if (msg.subject[0]) {
        snprintf(reply, sizeof(reply), "From %s\nSubj: %s\n\n%s", msg.fromName, msg.subject, msg.body);
    } else {
        snprintf(reply, sizeof(reply), "From %s:\n%s", msg.fromName, msg.body);
    }
    sendReplyMultipart(req, reply);
}

void BBSModule::doMailSend(const meshtastic_MeshPacket &req, const char *toStr, const char *subject, const char *body) {
    if (!toStr || toStr[0] == '\0') { sendReply(req, "No recipient."); return; }
    if (!body || body[0] == '\0') { sendReply(req, "Message is empty."); return; }
    if (strlen(body) > BBS_MSG_MAX_LEN) { sendReply(req, "Too long (200 char max)."); return; }

    uint32_t toNode = resolveNode(toStr);
    if (toNode == 0) {
        char err[80]; snprintf(err, sizeof(err), "'%s' not found.", toStr);
        sendReply(req, err); return;
    }

    BBSMailMsg msg;
    msg.id = storage_->nextMailId();
    msg.fromNode = req.from;
    msg.toNode = toNode;
    msg.timestamp = getTime();
    msg.read = false;
    msg.active = true;

    const char *name = getNodeShortName(req.from);
    if (name) strncpy(msg.fromName, name, BBS_SHORT_NAME_LEN - 1);
    else snprintf(msg.fromName, BBS_SHORT_NAME_LEN, "!%08x", req.from);
    msg.fromName[BBS_SHORT_NAME_LEN - 1] = '\0';

    memset(msg.subject, 0, BBS_SUBJECT_LEN);
    if (subject && subject[0]) {
        strncpy(msg.subject, subject, BBS_SUBJECT_LEN - 1);
    }

    strncpy(msg.body, body, BBS_MSG_MAX_LEN);
    msg.body[BBS_MSG_MAX_LEN] = '\0';

    if (storage_->storeMail(msg)) {
        char reply[80]; snprintf(reply, sizeof(reply), "Sent to %s!", toStr);
        sendReply(req, reply);
    } else {
        sendReply(req, "Failed to send.");
    }
}

void BBSModule::doMailDelete(const meshtastic_MeshPacket &req, uint32_t id) {
    if (storage_->deleteMail(id, req.from)) sendReply(req, "Deleted.");
    else sendReply(req, "Not found.");
}

void BBSModule::doQSLList(const meshtastic_MeshPacket &req, uint32_t pageNum) {
    uint32_t offset = (pageNum - 1) * PAGE_SIZE;
    BBSQSLHeader headers[PAGE_SIZE];
    uint32_t count = storage_->listQSL(headers, PAGE_SIZE, offset);

    if (count == 0) {
        sendReply(req, pageNum == 1 ? "QSL board empty.\nSend P to post!" : "No more entries.");
        return;
    }

    char reply[512] = {0};
    snprintf(reply, sizeof(reply), "QSL Board(pg%u):\n", pageNum);
    for (uint32_t i = 0; i < count; i++) {
        char line[100];
        snprintf(line, sizeof(line), "#%u %s %uhops%s\n",
                 headers[i].id, headers[i].fromName, headers[i].hopsAway,
                 headers[i].hasLocation ? " [GPS]" : "");
        strncat(reply, line, sizeof(reply) - strlen(reply) - 1);
    }
    strncat(reply, "[N]ext [P]ost mine", sizeof(reply) - strlen(reply) - 1);
    sendReplyMultipart(req, reply);
}

void BBSModule::doQSLPost(const meshtastic_MeshPacket &req) {
    storage_->pruneExpiredQSL(getTime());

    BBSQSL qsl;
    qsl.id = storage_->nextQSLId();
    qsl.fromNode = req.from;
    qsl.timestamp = getTime();
    qsl.hopsAway = (req.hop_start > req.hop_limit) ? (req.hop_start - req.hop_limit) : 0;
    qsl.latitude = qsl.longitude = qsl.altitude = 0;
    qsl.snr = (req.rx_snr > 15) ? 15 : (req.rx_snr < 0 ? 0 : (uint8_t)req.rx_snr);
    qsl.rssi = req.rx_rssi;
    qsl.active = true;

    const char *name = getNodeShortName(req.from);
    if (name) strncpy(qsl.fromName, name, BBS_SHORT_NAME_LEN - 1);
    else snprintf(qsl.fromName, BBS_SHORT_NAME_LEN, "!%08x", req.from);
    qsl.fromName[BBS_SHORT_NAME_LEN - 1] = '\0';

    if (storage_->storeQSL(qsl)) {
        char reply[120];
        snprintf(reply, sizeof(reply), "QSL #%u: %s heard %uhop(s) away SNR:%d",
                 qsl.id, qsl.fromName, qsl.hopsAway, req.rx_snr);
        if (isBroadcast(req.to)) sendToChannel(req, reply);
        else sendReply(req, reply);
    } else {
        sendReply(req, "Failed to post QSL.");
    }
}

void BBSModule::doStats(const meshtastic_MeshPacket &req) {
    BBSStats stats = storage_->getStats();
    char reply[220];
    snprintf(reply, sizeof(reply),
             "BBS Stats:\n"
             "Bulletins: %u/%u\n"
             "Mail: %u items\n"
             "QSL: %u posts\n"
             "Free: %u bytes",
             stats.totalBulletins, stats.maxBulletins,
             stats.totalMailItems, stats.totalQSLItems,
             stats.freeBytesEstimate);
    sendReply(req, reply);
}

// ─── Forecast ─────────────────────────────────────────────────────────────

// Extract a JSON string value — handles optional spaces around colon
static bool jsonExtractString(const char *json, const char *key, char *out, size_t outLen) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++; // skip ": " with any spacing
    if (*p != '"') return false;
    p++; // skip opening quote
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = end - p;
    if (len >= outLen) len = outLen - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

// Extract a JSON integer value — handles optional spaces around colon
static bool jsonExtractInt(const char *json, const char *key, int *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    *out = atoi(p);
    return true;
}

bool BBSModule::fetchForecast(char *buf, size_t bufLen) {
#ifndef ARCH_ESP32
    snprintf(buf, bufLen, "Forecast not supported on this platform.");
    return false;
#else
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(buf, bufLen, "No WiFi - forecast unavailable.");
        return false;
    }

    // Get GPS coordinates from this node
    const meshtastic_NodeInfoLite *myNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!myNode || !myNode->has_position ||
        (myNode->position.latitude_i == 0 && myNode->position.longitude_i == 0)) {
        snprintf(buf, bufLen, "No GPS fix - forecast unavailable.");
        return false;
    }
    float lat = myNode->position.latitude_i / 1e7f;
    float lon = myNode->position.longitude_i / 1e7f;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    // Step 1: NWS points lookup — read stream into a fixed buffer (response is ~5KB)
    char pointsUrl[96];
    snprintf(pointsUrl, sizeof(pointsUrl), "https://api.weather.gov/points/%.4f,%.4f", lat, lon);
    http.begin(client, pointsUrl);
    http.addHeader("User-Agent", "TinyBBS/1.0 (meshtastic)");
    int code = http.GET();
    if (code != 200) {
        snprintf(buf, bufLen, "NWS points error: %d", code);
        http.end();
        return false;
    }
    // Read up to 6KB — enough to find forecastHourly URL
    char pointsBuf[6144] = {0};
    WiFiClient *stream = http.getStreamPtr();
    size_t bytesRead = 0;
    uint32_t timeout = millis() + 5000;
    while (bytesRead < sizeof(pointsBuf) - 1 && millis() < timeout) {
        if (stream->available()) {
            pointsBuf[bytesRead++] = stream->read();
        }
    }
    http.end();

    char forecastUrl[160] = {0};
    if (!jsonExtractString(pointsBuf, "forecastHourly", forecastUrl, sizeof(forecastUrl))) {
        printf("[BBS] forecastHourly not found in: %.200s\n", pointsBuf);
        snprintf(buf, bufLen, "NWS: no forecast URL.");
        return false;
    }
    printf("[BBS] forecastUrl: %s\n", forecastUrl);

    // Step 2: Stream hourly forecast — scan for T06: and T18: periods
    // Response is ~200KB so we read in chunks, never load it all into RAM
    http.begin(client, forecastUrl);
    http.addHeader("User-Agent", "TinyBBS/1.0 (meshtastic)");
    code = http.GET();
    if (code != 200) {
        snprintf(buf, bufLen, "NWS forecast error: %d", code);
        http.end();
        return false;
    }

    char am_str[80] = {0};
    char pm_str[80] = {0};

    // Rolling buffer: read stream, keep last 512 bytes to search across chunk boundaries
    static const size_t CHUNK = 512;
    char roll[1024] = {0}; // double-chunk rolling window
    size_t rollLen = 0;
    stream = http.getStreamPtr();
    timeout = millis() + 15000;

    while ((am_str[0] == '\0' || pm_str[0] == '\0') && millis() < timeout) {
        // Fill second half of rolling buffer
        size_t got = 0;
        while (got < CHUNK && millis() < timeout) {
            if (stream->available()) {
                roll[rollLen + got] = stream->read();
                got++;
            }
        }
        if (got == 0) break;
        rollLen += got;
        roll[rollLen] = '\0';

        // Search for T06: or T18: in this window
        for (int pass = 0; pass < 2; pass++) {
            const char *marker = pass == 0 ? "T06:" : "T18:";
            char *found = strstr(roll, marker);
            if (!found) continue;

            // Check this is inside a startTime value — look back for "startTime"
            char *stKey = (char *)roll;
            char *lastST = nullptr;
            while ((stKey = strstr(stKey, "startTime")) != nullptr && stKey < found) {
                lastST = stKey;
                stKey++;
            }
            if (!lastST) continue;

            // Extract a block from the startTime key forward (~350 chars)
            char block[360] = {0};
            strncpy(block, lastST, 350);
            block[350] = '\0';

            int temp = 0;
            char unit[4] = "F";
            char wind[16] = "?";
            char sky[24] = "?";
            jsonExtractInt(block, "temperature", &temp);
            jsonExtractString(block, "temperatureUnit", unit, sizeof(unit));
            jsonExtractString(block, "windSpeed", wind, sizeof(wind));
            jsonExtractString(block, "shortForecast", sky, sizeof(sky));
            sky[20] = '\0';

            if (pass == 0 && am_str[0] == '\0')
                snprintf(am_str, sizeof(am_str), "6am: %d\xC2\xB0%s %s %s", temp, unit, sky, wind);
            if (pass == 1 && pm_str[0] == '\0')
                snprintf(pm_str, sizeof(pm_str), "6pm: %d\xC2\xB0%s %s %s", temp, unit, sky, wind);
        }

        // Slide window: keep last CHUNK bytes
        if (rollLen >= CHUNK) {
            memmove(roll, roll + rollLen - CHUNK, CHUNK);
            rollLen = CHUNK;
        }
    }
    http.end();

    if (!am_str[0] && !pm_str[0]) {
        snprintf(buf, bufLen, "No forecast data available.");
        return false;
    }

    snprintf(buf, bufLen, "TinyBBS Forecast:\n%s\n%s",
             am_str[0] ? am_str : "6am: N/A",
             pm_str[0] ? pm_str : "6pm: N/A");
    return true;
#endif
}

void BBSModule::doForecast(const meshtastic_MeshPacket &req) {
    sendReply(req, "Fetching forecast...");
    char buf[200] = {0};
    fetchForecast(buf, sizeof(buf));
    sendReply(req, buf[0] ? buf : "Forecast unavailable.");
}

// ─── Reply helpers ────────────────────────────────────────────────────────

bool BBSModule::sendToChannel(const meshtastic_MeshPacket &req, const char *text) {
    if (!text) return false;
    meshtastic_MeshPacket *reply = allocDataPacket();
    reply->to = NODENUM_BROADCAST;
    reply->channel = req.channel;
    reply->want_ack = false;
    reply->decoded.want_response = false;
    size_t len = strlen(text);
    if (len > REPLY_MAX_LEN) len = REPLY_MAX_LEN;
    reply->decoded.payload.size = len;
    memcpy(reply->decoded.payload.bytes, text, len);
    service->sendToMesh(reply);
    return true;
}

bool BBSModule::sendReply(const meshtastic_MeshPacket &req, const char *text) {
    if (!text) return false;
    meshtastic_MeshPacket *reply = allocDataPacket();
    reply->to = req.from;
    reply->channel = req.channel;
    reply->want_ack = false;
    reply->decoded.want_response = false;
    size_t len = strlen(text);
    if (len > REPLY_MAX_LEN) len = REPLY_MAX_LEN;
    reply->decoded.payload.size = len;
    memcpy(reply->decoded.payload.bytes, text, len);
    service->sendToMesh(reply);
    return true;
}

bool BBSModule::sendReplyMultipart(const meshtastic_MeshPacket &req, const char *text) {
    if (!text) return false;
    size_t totalLen = strlen(text);
    size_t sentLen = 0;
    while (sentLen < totalLen) {
        size_t chunkLen = std::min((size_t)REPLY_MAX_LEN, totalLen - sentLen);
        if (chunkLen == REPLY_MAX_LEN) {
            for (size_t i = chunkLen; i > chunkLen / 2; i--) {
                if (text[sentLen + i - 1] == '\n') { chunkLen = i; break; }
            }
        }
        meshtastic_MeshPacket *reply = allocDataPacket();
        reply->to = req.from;
        reply->channel = req.channel;
        reply->want_ack = false;
        reply->decoded.want_response = false;
        reply->decoded.payload.size = chunkLen;
        memcpy(reply->decoded.payload.bytes, text + sentLen, chunkLen);
        service->sendToMesh(reply);
        sentLen += chunkLen;
        if (sentLen < totalLen) delay(MULTIPART_DELAY_MS);
    }
    return true;
}

// ─── Node helpers ─────────────────────────────────────────────────────────

uint32_t BBSModule::resolveNode(const char *idOrName) {
    if (!idOrName || idOrName[0] == '\0') return 0;
    if (idOrName[0] == '!') {
        uint32_t nodeNum = 0;
        sscanf(idOrName + 1, "%x", &nodeNum);
        if (nodeNum != 0) return nodeNum;
    }
    uint32_t numNodes = nodeDB->getNumMeshNodes();
    for (uint32_t i = 0; i < numNodes; i++) {
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (node && node->has_user && node->user.short_name[0] != '\0') {
            if (strcasecmp(node->user.short_name, idOrName) == 0) return node->num;
        }
    }
    return 0;
}

const char *BBSModule::getNodeShortName(uint32_t nodeNum) {
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeNum);
    if (node && node->has_user && node->user.short_name[0] != '\0') return node->user.short_name;
    return nullptr;
}
