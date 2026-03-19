#pragma once

#include <cstdint>
#include <cstring>

// Message size constraints
#define BBS_MSG_MAX_LEN 200
#define BBS_SHORT_NAME_LEN 12
#define BBS_PREVIEW_LEN 40
#define BBS_SUBJECT_LEN 32

// Storage profiles
#ifdef ARCH_ESP32
#define PSRAM_MAX_BULLETINS 500
#define PSRAM_MAX_MAILBOXES 128
#define PSRAM_MAX_MAIL_PER_USER 100
#else
#define PSRAM_MAX_BULLETINS 500
#define PSRAM_MAX_MAILBOXES 128
#define PSRAM_MAX_MAIL_PER_USER 100
#endif

#define FLASH_MAX_BULLETINS 50
#define FLASH_MAX_MAILBOXES 32
#define FLASH_MAX_MAIL_PER_USER 20

// QSL board capacity (ephemeral, recent heard list)
#define PSRAM_MAX_QSL 200
#define FLASH_MAX_QSL 50
#define QSL_TTL_SECONDS (86400) // Keep QSL posts for 24 hours

// Wordle daily score tracking
#define BBS_WORDLE_MAX_SCORES 20

/**
 * Bulletin board categories (matching TC2 BBS)
 */
enum BBSBoard : uint8_t {
    BOARD_GENERAL = 0,
    BOARD_INFO    = 1,
    BOARD_NEWS    = 2,
    BOARD_URGENT  = 3,
    BOARD_COUNT   = 4,
    BOARD_ALL     = 0xFF,
};

/**
 * Bulletin structure - stores a single bulletin post
 */
struct BBSBulletin {
    uint32_t id;
    uint32_t authorNode;
    char authorName[BBS_SHORT_NAME_LEN];
    uint32_t timestamp;
    char body[BBS_MSG_MAX_LEN + 1];
    uint8_t board; // BBSBoard value, appended last for backward compat
    bool active;
};

/**
 * Bulletin header for listings
 */
struct BBSBulletinHeader {
    uint32_t id;
    uint32_t authorNode;
    char authorName[BBS_SHORT_NAME_LEN];
    uint32_t timestamp;
    char preview[BBS_PREVIEW_LEN + 1];
    uint8_t board;
};

/**
 * Mail message structure
 */
struct BBSMailMsg {
    uint32_t id;
    uint32_t fromNode;
    char fromName[BBS_SHORT_NAME_LEN];
    uint32_t toNode;
    uint32_t timestamp;
    char subject[BBS_SUBJECT_LEN];
    char body[BBS_MSG_MAX_LEN + 1];
    bool read;
    bool active;
};

/**
 * Mail header for listings
 */
struct BBSMailHeader {
    uint32_t id;
    uint32_t fromNode;
    char fromName[BBS_SHORT_NAME_LEN];
    uint32_t timestamp;
    char subject[BBS_SUBJECT_LEN];
    bool read;
};

/**
 * QSL (confirmation) record
 */
struct BBSQSL {
    uint32_t id;
    uint32_t fromNode;
    char fromName[BBS_SHORT_NAME_LEN];
    int32_t latitude;
    int32_t longitude;
    int32_t altitude;
    uint32_t timestamp;
    uint8_t hopsAway;
    uint8_t snr;
    int8_t rssi;
    bool active;
};

/**
 * QSL header for listings
 */
struct BBSQSLHeader {
    uint32_t id;
    uint32_t fromNode;
    char fromName[BBS_SHORT_NAME_LEN];
    uint32_t timestamp;
    uint8_t hopsAway;
    bool hasLocation;
};

/**
 * Wordle daily score record
 * guesses: 1-6 = solved in that many guesses, 0 = DNF (used all 6, didn't solve)
 */
struct BBSWordleScore {
    uint32_t nodeNum;
    char shortName[BBS_SHORT_NAME_LEN];
    uint8_t guesses;
};

/**
 * Storage statistics
 */
struct BBSStats {
    uint32_t freeBytesEstimate;
    uint32_t totalBulletins;
    uint32_t totalMailItems;
    uint32_t totalQSLItems;
    uint32_t maxBulletins;
    uint32_t maxMailPerUser;
};

/**
 * Abstract storage interface
 */
class BBSStorage {
  public:
    virtual ~BBSStorage() {}
    virtual bool init() = 0;

    // --- BULLETIN OPERATIONS ---
    virtual bool storeBulletin(const BBSBulletin &bulletin) = 0;
    virtual bool loadBulletin(uint32_t id, BBSBulletin &bulletin) = 0;
    virtual bool deleteBulletin(uint32_t id, uint32_t requestorNodeNum) = 0;
    // board=BOARD_ALL lists all boards; otherwise filters to that board
    virtual uint32_t listBulletins(BBSBulletinHeader *headers, uint32_t maxResults, uint32_t offset = 0, uint8_t board = BOARD_ALL) = 0;
    virtual uint32_t totalActiveBulletins(uint8_t board = BOARD_ALL) = 0;

    // --- MAIL OPERATIONS ---
    virtual bool storeMail(const BBSMailMsg &msg) = 0;
    virtual bool loadMail(uint32_t id, uint32_t recipientNode, BBSMailMsg &msg) = 0;
    virtual bool deleteMail(uint32_t id, uint32_t recipientNode) = 0;
    virtual bool markMailRead(uint32_t id, uint32_t recipientNode) = 0;
    virtual uint32_t listMail(uint32_t recipientNode, BBSMailHeader *headers, uint32_t maxResults, uint32_t offset = 0) = 0;
    virtual uint32_t countUnreadMail(uint32_t recipientNode) = 0;

    // --- HOUSEKEEPING ---
    virtual bool compact() = 0;
    virtual BBSStats getStats() = 0;
    virtual uint32_t nextBulletinId() = 0;
    virtual uint32_t nextMailId() = 0;

    // --- QSL OPERATIONS ---
    virtual bool storeQSL(const BBSQSL &qsl) = 0;
    virtual uint32_t listQSL(BBSQSLHeader *headers, uint32_t maxResults, uint32_t offset = 0) = 0;
    virtual uint32_t totalActiveQSL() = 0;
    virtual uint32_t pruneExpiredQSL(uint32_t currentTime) = 0;
    virtual uint32_t nextQSLId() = 0;

    // Wordle score persistence is handled directly by BBSModule via FSCom
    // (always LittleFS, always persistent regardless of main storage backend)
};
