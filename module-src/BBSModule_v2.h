#pragma once

#include "BBSStorage.h"
#include "concurrency/OSThread.h"
#include "mesh/SinglePortModule.h"

static constexpr uint8_t BBS_MAX_SESSIONS = 8;
static constexpr uint32_t BBS_SESSION_TIMEOUT_S = 600;

enum BBSMenuState : uint8_t {
    BBS_STATE_IDLE = 0,
    BBS_STATE_MAIN,
    BBS_STATE_BULLETIN,        // board selection
    BBS_STATE_BULLETIN_BOARD,  // within a board
    BBS_STATE_MAIL,
    BBS_STATE_MAIL_SEND_TO,
    BBS_STATE_MAIL_SEND_SUBJECT,
    BBS_STATE_MAIL_SEND_BODY,
    BBS_STATE_QSL,
    BBS_STATE_WORDLE,
};

struct BBSSession {
    uint32_t nodeNum;
    BBSMenuState state;
    uint32_t lastActivity;
    char mailSendTo[32];
    char mailSendSubject[BBS_SUBJECT_LEN];
    uint8_t currentBoard;   // BBSBoard value
    uint32_t bulletinPage;
    uint32_t qslPage;
    // Wordle game state
    char wordleTarget[6];   // current target word
    uint8_t wordleGuesses;  // guesses used (0-6)
    uint8_t wordleGamesPlayed; // for seed variation
    uint32_t wordleDay;     // day number this game started (for score saving)
};

/**
 * Meshtastic BBS Module - TC2-style menu interface
 *
 * DMs: any message enters the session state machine (no !bbs prefix needed)
 * Channel: requires !bbs prefix for one-shot commands
 */
class BBSModule : public SinglePortModule, private concurrency::OSThread {
  private:
    BBSStorage *storage_ = nullptr;
    BBSSession sessions_[BBS_MAX_SESSIONS];

    static constexpr size_t REPLY_MAX_LEN = 200;
    static constexpr uint32_t PAGE_SIZE = 5;
    static constexpr uint32_t MULTIPART_DELAY_MS = 1500;

    // Board names (index = BBSBoard value)
    static const char *boardName(uint8_t board);

    // Session management
    BBSSession *getOrCreateSession(uint32_t nodeNum);
    void expireSessions(uint32_t now);

    // Menu display
    void sendMainMenu(const meshtastic_MeshPacket &req, BBSSession &session);
    void sendBoardSelectMenu(const meshtastic_MeshPacket &req);
    void sendBulletinMenu(const meshtastic_MeshPacket &req, uint8_t board);
    void sendMailMenu(const meshtastic_MeshPacket &req, uint32_t nodeNum);
    void sendQSLMenu(const meshtastic_MeshPacket &req);

    // Message type handlers
    ProcessMessage handleDM(const meshtastic_MeshPacket &mp, const char *text);
    ProcessMessage handleChannelCmd(const meshtastic_MeshPacket &mp, const char *cmd);

    // State machine
    ProcessMessage dispatchState(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateMain(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateBulletin(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateBulletinBoard(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateMail(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateMailSendTo(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateMailSendSubject(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateMailSendBody(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateQSL(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateWordle(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    void doWordleStart(const meshtastic_MeshPacket &req, BBSSession &session);

    // Action implementations
    void doBulletinList(const meshtastic_MeshPacket &req, uint32_t pageNum, uint8_t board);
    void doBulletinRead(const meshtastic_MeshPacket &req, uint32_t id);
    void doBulletinPost(const meshtastic_MeshPacket &req, const char *text, uint8_t board);
    void doBulletinDelete(const meshtastic_MeshPacket &req, uint32_t id);
    void doMailList(const meshtastic_MeshPacket &req, uint32_t recipientNode);
    void doMailRead(const meshtastic_MeshPacket &req, uint32_t id);
    void doMailSend(const meshtastic_MeshPacket &req, const char *toStr, const char *subject, const char *body);
    void doMailDelete(const meshtastic_MeshPacket &req, uint32_t id);
    void doQSLList(const meshtastic_MeshPacket &req, uint32_t pageNum);
    void doQSLPost(const meshtastic_MeshPacket &req);
    void doStats(const meshtastic_MeshPacket &req);
    void doForecast(const meshtastic_MeshPacket &req);
    bool fetchForecast(char *buf, size_t bufLen);

    // Helpers
    bool sendReply(const meshtastic_MeshPacket &req, const char *text);
    bool sendToChannel(const meshtastic_MeshPacket &req, const char *text);
    bool sendReplyMultipart(const meshtastic_MeshPacket &req, const char *text);
    uint32_t resolveNode(const char *idOrName);
    const char *getNodeShortName(uint32_t nodeNum);

    // Wordle helpers - score persistence via LittleFS directly (always persistent)
    static uint32_t wordleDay();  // current day number (UTC 7am boundary)
    static void wordleEnsureDir();
    static bool wordleHasPlayed(uint32_t day, uint32_t nodeNum);
    static bool wordleSaveScore(uint32_t day, const BBSWordleScore &score);
    static uint32_t wordleLoadScores(uint32_t day, BBSWordleScore *scores, uint32_t max);
    static void wordlePruneOldDays(uint32_t currentDay);
    void buildStandings(uint32_t day, char *buf, size_t bufLen);
    void sendToPublicChannel(const char *text);

    // Daily announcement tracking
    uint32_t lastAnnouncedDay_ = 0;
    uint8_t lastForecastHour_ = 255; // 255 = not yet fired this hour

    // OSThread periodic task (private inheritance)
    virtual int32_t runOnce() override;

  public:
    BBSModule();
    virtual ~BBSModule();
    virtual void setup() override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
};

extern BBSModule *bbsModule;
