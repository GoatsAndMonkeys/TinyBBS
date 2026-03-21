#pragma once

#include "BBSStorage.h"
#include "FalloutWastelandRPG.h"
#include "concurrency/OSThread.h"
#include "graphics/Screen.h"
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
    BBS_STATE_GAMES,            // Games sub-menu
    BBS_STATE_WORDLE,
    BBS_STATE_VAULT,            // Vault-Tec hacking game
    BBS_STATE_WASTELAND,        // Fallout Wasteland RPG
    BBS_STATE_CASINO,           // Casino sub-menu
    BBS_STATE_BLACKJACK,        // Blackjack mid-hand
    BBS_STATE_ROULETTE,         // Roulette (awaiting bet type)
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
    // Vault-Tec hacking game state
    char     vaultWords[12][6]; // 12 displayed words (lowercase)
    uint8_t  vaultAnswer;       // index 0-11 of correct word
    uint8_t  vaultGuesses;      // attempts remaining (starts at 5)
    // Casino state
    uint8_t  casinoMode;        // 0=standalone play money, 1=Vegas (real caps)
    uint16_t casinoChips;       // current chip count
    uint8_t  bjPlayerVals[6];   // player card values (1-13)
    uint8_t  bjPlayerCount;
    uint8_t  bjDealerVals[6];   // dealer card values
    uint8_t  bjDealerCount;
    uint8_t  bjDoubled;         // 1 if player doubled down
    uint8_t  rlBetType;         // 1=red,2=black,3=odd,4=even,5=high,6=low,7=number
    uint8_t  rlBetNum;          // 1-36 if rlBetType==7
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
    ProcessMessage handleStateGames(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateVault(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateWasteland(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateCasino(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateBlackjack(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    ProcessMessage handleStateRoulette(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text);
    void sendGamesMenu(const meshtastic_MeshPacket &req);
    void sendCasinoMenu(const meshtastic_MeshPacket &req, const BBSSession &session);
    void doSlots(const meshtastic_MeshPacket &req, BBSSession &session);
    void doWordleStart(const meshtastic_MeshPacket &req, BBSSession &session);
    void doVaultStart(const meshtastic_MeshPacket &req, BBSSession &session);
    void sendVaultBoard(const meshtastic_MeshPacket &req, const BBSSession &session);

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
    bool fetchForecast(char *buf, size_t bufLen, float lat, float lon);
    bool reverseGeocode(float lat, float lon, char *city, size_t cityLen);

    // Helpers
    bool sendReply(const meshtastic_MeshPacket &req, const char *text);
    bool sendToChannel(const meshtastic_MeshPacket &req, const char *text);
    bool sendReplyMultipart(const meshtastic_MeshPacket &req, const char *text);
    void sendTapBack(const meshtastic_MeshPacket &req, uint8_t hops);
    uint32_t resolveNode(const char *idOrName);
    const char *getNodeShortName(uint32_t nodeNum);

    // Wordle helpers - score persistence via LittleFS directly (always persistent)
    uint32_t wordleDay();  // current day number (9am local boundary)
    static void wordleEnsureDir();
    static bool wordleHasPlayed(uint32_t day, uint32_t nodeNum);
    static bool wordleSaveScore(uint32_t day, const BBSWordleScore &score);
    static uint32_t wordleLoadScores(uint32_t day, BBSWordleScore *scores, uint32_t max);
    static void wordlePruneOldDays(uint32_t currentDay);
    void buildStandings(uint32_t day, char *buf, size_t bufLen);
    void sendToPublicChannel(const char *text);

    // Daily announcement tracking
    uint32_t lastAnnouncedDay_ = 0;
    uint8_t lastForecastHour_ = 255;
    uint8_t lastNightlyHour_ = 255;
    int32_t utcOffsetSeconds_ = -14400;  // default EDT (UTC-4); updated from Open-Meteo

    // Forecast cache — shared across all users, refreshed at most every 5 min
    static constexpr uint32_t FORECAST_CACHE_TTL_S = 300;
    uint32_t lastForecastFetchTime_ = 0;
    char forecastCache_[200] = {0};

    // UI frame stats — counts updated by runOnce(), drawFrame() just reads them
    uint32_t uiLastMsgTime_ = 0;
    char     uiLastMsgFrom_[12] = {0};
    uint32_t uiBulletinTotal_ = 0;
    uint32_t uiBulletinRecent_ = 0;   // last 7 days
    uint32_t uiMailTotal_ = 0;
    uint32_t uiStatsLastUpdate_ = 0;  // timestamp of last count refresh

    // OSThread periodic task (private inheritance)
    virtual int32_t runOnce() override;

  public:
    BBSModule();
    virtual ~BBSModule();
    virtual void setup() override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    // OLED UI frame
    virtual bool wantUIFrame() override { return true; }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
};

extern BBSModule *bbsModule;
