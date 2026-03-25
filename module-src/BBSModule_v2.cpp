#include "BBSModule_v2.h"
#include "BBSWordle.h"
#include "BBSStorageLittleFS.h"
#include "BBSStoragePSRAM.h"
#include "BBSGeoLookup.h"
#ifdef NRF52_SERIES
#include "BBSStorageExtFlash.h"
#include "BBSExtFlash.h"
#ifdef BBS_KB_LOADER
#include "BBSKBLoader.h"
#endif
#endif
#include "Channels.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "modules/RoutingModule.h"
#include "airtime.h"
#include "RTC.h"
#include <Arduino.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#ifdef ARCH_ESP32
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#endif

BBSModule *bbsModule;

static const char *const BOARD_NAMES[BOARD_COUNT] = {"General", "Info", "News", "Urgent"};
static const char BOARD_KEYS[BOARD_COUNT] = {'g', 'i', 'n', 'u'};

// ─── Constructor / Destructor ─────────────────────────────────────────────

BBSModule::BBSModule() : SinglePortModule("bbs", meshtastic_PortNum_TEXT_MESSAGE_APP),
                         concurrency::OSThread("bbs_daily") {
    memset(sessions_, 0, sizeof(sessions_));

#ifdef ARCH_ESP32
    bool initOk = false;
    if (ESP.getFreePsram() > 1024 * 1024) {
        storage_ = new BBSStoragePSRAM();
        if (storage_) {
            initOk = storage_->init();
            if (!initOk) {
                delete storage_;
                storage_ = nullptr;
            }
        }
    }
    if (!storage_) {
        storage_ = new BBSStorageLittleFS();
        if (storage_) initOk = storage_->init();
    }
    frpgEnsureDir();
#endif
    // nRF52: storage initialized lazily on first message to avoid blocking boot

#if defined(NRF52_SERIES) && defined(MESHTASTIC_EXCLUDE_SCREEN)
    tinyScreen_.begin();
#endif
}

#ifdef NRF52_SERIES
#endif

void BBSModule::setup() {
    LOG_DEBUG("[BBS] setup() called\n");
#ifndef NRF52_SERIES
    wordleEnsureDir(); // ensure /bbs/wdl/ exists for Wordle score persistence
    frpgEnsureDir();   // ensure /bbs/frpg/ exists for Wasteland RPG
#endif
}

#ifdef NRF52_SERIES
// Simple base64 decode (in-place, returns decoded length)
static size_t b64decode(const char *in, uint8_t *out, size_t maxOut) {
    static const uint8_t T[128] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
        52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
        64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64
    };
    size_t len = strlen(in), o = 0;
    uint32_t buf = 0; int bits = 0;
    for (size_t i = 0; i < len && o < maxOut; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c == '=' || c >= 128 || T[c] == 64) continue;
        buf = (buf << 6) | T[c];
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (buf >> bits) & 0xFF; }
    }
    return o;
}

void BBSModule::handleKBUpload(const char *cmd) {
    using namespace Adafruit_LittleFS_Namespace;

    if (strncmp(cmd, "OPEN ", 5) == 0) {
        char path[64] = {0};
        uint32_t size = 0;
        if (sscanf(cmd + 5, "%63s %u", path, &size) != 2) {
            LOG_WARN("[KB] Bad OPEN args\n");
            return;
        }
        if (kbFile_) { ((File *)kbFile_)->close(); delete (File *)kbFile_; kbFile_ = nullptr; }
        if (!bbsExtFS().exists("/bbs")) bbsExtFS().mkdir("/bbs");
        if (!bbsExtFS().exists("/bbs/kb")) bbsExtFS().mkdir("/bbs/kb");
        if (bbsExtFS().exists(path)) bbsExtFS().remove(path);
        File f = bbsExtFS().open(path, FILE_O_WRITE);
        if (!f) { LOG_ERROR("[KB] Can't open %s\n", path); return; }
        kbFile_ = new File(f);
        kbExpected_ = size;
        kbReceived_ = 0;
        LOG_INFO("[KB] OPEN %s (%u bytes)\n", path, size);
    }
    else if (strncmp(cmd, "DATA ", 5) == 0) {
        if (!kbFile_) return;
        uint8_t decoded[200];
        size_t n = b64decode(cmd + 5, decoded, sizeof(decoded));
        if (n > 0) {
            ((File *)kbFile_)->write(decoded, n);
            kbReceived_ += n;
        }
    }
    else if (strncmp(cmd, "CLOSE", 5) == 0) {
        if (kbFile_) {
            ((File *)kbFile_)->close();
            delete (File *)kbFile_;
            kbFile_ = nullptr;
            LOG_INFO("[KB] CLOSE %u/%u bytes\n", kbReceived_, kbExpected_);
        }
    }
    else if (strncmp(cmd, "LIST", 4) == 0) {
        File dir = bbsExtFS().open("/bbs/kb", FILE_O_READ);
        if (dir) {
            File f(bbsExtFS());
            while ((f = dir.openNextFile())) {
                LOG_INFO("[KB] %s %u\n", f.name(), (uint32_t)f.size());
                f.close();
            }
            dir.close();
        }
    }
}
#endif

#ifdef NRF52_SERIES
// Called from StreamAPI when it sees a 0xBB byte — we handle the full frame here
static void *_kbFile = nullptr;
static uint32_t _kbExpected = 0, _kbReceived = 0;

void bbsSerialFrameHandler(Stream *stream, uint8_t firstByte) {
    using namespace Adafruit_LittleFS_Namespace;

    // Read rest of header: cmd(1) + len(2)
    uint8_t hdr[3];
    if (stream->readBytes(hdr, 3) != 3) return;
    uint8_t cmd = hdr[0];
    uint16_t dataLen = hdr[1] | (hdr[2] << 8);
    if (dataLen > 512) return;

    // Read payload + CRC
    uint8_t payload[514];
    if (stream->readBytes(payload, dataLen + 1) != (size_t)(dataLen + 1)) return;

    // CRC check
    uint8_t crcData[4] = {firstByte, cmd, hdr[1], hdr[2]};
    uint8_t crc = 0;
    for (int i = 0; i < 4; i++) { crc ^= crcData[i]; for (int j = 0; j < 8; j++) crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1); }
    for (uint16_t i = 0; i < dataLen; i++) { crc ^= payload[i]; for (int j = 0; j < 8; j++) crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1); }
    if (crc != payload[dataLen]) {
        uint8_t resp[3] = {0xBB, 0x80, 1};
        stream->write(resp, 3);
        return;
    }

    uint8_t status = 1; // default ERR

    if (cmd == 0x01 && dataLen >= 6) { // OPEN
        uint8_t pathLen = payload[0];
        char path[64] = {0};
        memcpy(path, payload + 1, pathLen < 63 ? pathLen : 63);
        uint32_t fsize;
        memcpy(&fsize, payload + 1 + pathLen, 4);

        if (_kbFile) { ((File *)_kbFile)->close(); delete (File *)_kbFile; _kbFile = nullptr; }
        if (!bbsExtFS().exists("/bbs")) bbsExtFS().mkdir("/bbs");
        if (!bbsExtFS().exists("/bbs/kb")) bbsExtFS().mkdir("/bbs/kb");
        if (bbsExtFS().exists(path)) bbsExtFS().remove(path);
        File f = bbsExtFS().open(path, FILE_O_WRITE);
        if (f) {
            _kbFile = new File(f);
            _kbExpected = fsize;
            _kbReceived = 0;
            status = 0;
        }
    } else if (cmd == 0x02 && _kbFile) { // DATA
        uint8_t ramBuf[256];
        uint16_t pos = 0;
        while (pos < dataLen) {
            uint16_t chunk = dataLen - pos;
            if (chunk > sizeof(ramBuf)) chunk = sizeof(ramBuf);
            memcpy(ramBuf, payload + pos, chunk);
            ((File *)_kbFile)->write(ramBuf, chunk);
            pos += chunk;
        }
        _kbReceived += dataLen;
        status = 0;
    } else if (cmd == 0x03) { // CLOSE
        if (_kbFile) { ((File *)_kbFile)->close(); delete (File *)_kbFile; _kbFile = nullptr; }
        status = 0;
    } else if (cmd == 0x04) { // LIST
        status = 0;
    }

    uint8_t resp[3] = {0xBB, 0x80, status};
    stream->write(resp, 3);
    ((Stream *)stream)->flush();
}
#endif

int32_t BBSModule::runOnce() {
    uint32_t t = getTime();

#ifdef NRF52_SERIES
    // Poll for serial upload frames (0xBB prefix, ignored by Meshtastic)
    if (Serial.available() && Serial.peek() == 0xBB) {
        using namespace Adafruit_LittleFS_Namespace;
        uint8_t hdr[4];
        Serial.readBytes(hdr, 4);
        uint8_t cmd = hdr[1];
        uint16_t dataLen = hdr[2] | (hdr[3] << 8);
        if (dataLen <= 512) {
            uint8_t payload[514];
            size_t got = Serial.readBytes(payload, dataLen + 1); // +1 for CRC
            if (got == dataLen + 1) {
                uint8_t resp[3] = {0xBB, 0x80, 1}; // default ERR
                if (cmd == 0x01 && dataLen >= 6) { // OPEN
                    uint8_t pathLen = payload[0];
                    char path[64] = {0};
                    memcpy(path, payload + 1, pathLen < 63 ? pathLen : 63);
                    uint32_t fsize;
                    memcpy(&fsize, payload + 1 + pathLen, 4);
                    if (kbFile_) { ((File*)kbFile_)->close(); delete (File*)kbFile_; kbFile_ = nullptr; }
                    if (!bbsExtFS().exists("/bbs")) bbsExtFS().mkdir("/bbs");
                    if (!bbsExtFS().exists("/bbs/kb")) bbsExtFS().mkdir("/bbs/kb");
                    if (bbsExtFS().exists(path)) bbsExtFS().remove(path);
                    File f = bbsExtFS().open(path, FILE_O_WRITE);
                    if (f) { kbFile_ = new File(f); kbExpected_ = fsize; kbReceived_ = 0; resp[2] = 0; }
                } else if (cmd == 0x02 && kbFile_) { // DATA
                    ((File*)kbFile_)->write(payload, dataLen);
                    kbReceived_ += dataLen;
                    resp[2] = 0;
                } else if (cmd == 0x03) { // CLOSE
                    if (kbFile_) { ((File*)kbFile_)->close(); delete (File*)kbFile_; kbFile_ = nullptr; }
                    resp[2] = 0;
                }
                Serial.write(resp, 3);
            }
        }
        return 100; // poll fast during upload
    }
#endif
    // Wait until time is synced (must be after 2020-01-01)
    if (t < 1577836800UL) return 60000;

    // Refresh UI frame stats every 60 seconds
    // Skip on nRF52: FS ops in runOnce() cause deadlocks/crashes with external flash
#ifndef NRF52_SERIES
    if (storage_ && (t - uiStatsLastUpdate_) >= 60) {
        uiStatsLastUpdate_ = t;
        BBSStats stats = storage_->getStats();
        uiMailTotal_      = stats.totalMailItems;
        uiBulletinTotal_  = stats.totalBulletins;
        // Count bulletins from last 7 days
        static BBSBulletinHeader bhdrs[200];
        uint32_t n = storage_->listBulletins(bhdrs, 200);
        uint32_t recent = 0;
        uint32_t week = 7 * 86400UL;
        for (uint32_t i = 0; i < n; i++)
            if (t >= bhdrs[i].timestamp && (t - bhdrs[i].timestamp) < week) recent++;
        uiBulletinRecent_ = recent;
    }
#endif

#if defined(NRF52_SERIES) && defined(MESHTASTIC_EXCLUDE_SCREEN)
    {
        // Count active sessions
        uint8_t activeSess = 0;
        for (int i = 0; i < BBS_MAX_SESSIONS; i++)
            if (sessions_[i].nodeNum != 0) activeSess++;

        // Get radio stats
        uint32_t rxCount = 0, txCount = 0;
        float airUtil = 0;
        if (airTime) airUtil = airTime->channelUtilizationPercent();

        // Get node count
        uint16_t nodesOnline = nodeDB ? (uint16_t)nodeDB->getNumOnlineMeshNodes() : 0;

        // Get storage free bytes
        uint32_t freeBytes = 0;
        if (storage_) {
            BBSStats stats = storage_->getStats();
            freeBytes = stats.freeBytesEstimate;
        }

        tinyScreen_.refresh(t, storage_, activeSess,
                           rxCount, txCount, airUtil,
                           nodesOnline, freeBytes);
    }
#endif

    uint32_t day = wordleDay();

    if (lastAnnouncedDay_ == 0) {
        // First valid time read — set baseline, seed forecast hour to skip current hour
        lastAnnouncedDay_ = day;
        // Seed with current local hour so we don't fire immediately on boot
        // (utcOffsetSeconds_ may be 0 until first forecast fetch, which is acceptable)
        uint32_t seedLocalT = (uint32_t)((int64_t)t + utcOffsetSeconds_);
        lastForecastHour_ = (seedLocalT % 86400) / 3600;
        lastNightlyHour_  = (seedLocalT % 86400) / 3600;
        LOG_DEBUG("[BBS] runOnce: time synced, wordle day=%u\n", day);
        return 60000;
    }

#ifndef NRF52_SERIES
    if (day > lastAnnouncedDay_) {
        // New wordle day - announce and show yesterday's standings
        char msg[200] = {0};
        char standings[160] = {0};
        buildStandings(lastAnnouncedDay_, standings, sizeof(standings));

        if (strncmp(standings, "No Wordle", 9) == 0) {
            snprintf(msg, sizeof(msg), "New Wordle word ready!\nDM [W] to play.");
        } else {
            snprintf(msg, sizeof(msg), "New Wordle word!\n%s\nDM [W] to play.", standings);
        }
        sendToPublicChannel(msg);

        wordlePruneOldDays(day);
        lastAnnouncedDay_ = day;
        LOG_DEBUG("[BBS] runOnce: day transition to %u, announced.\n", day);
    }
#else
    lastAnnouncedDay_ = day; // track day even on nRF52 (no announcement)
#endif

    // Use local time for scheduled broadcasts (utcOffsetSeconds_ updated by fetchForecast)
    uint32_t localT = (uint32_t)((int64_t)t + utcOffsetSeconds_);
    uint8_t localHour = (localT % 86400) / 3600;

#ifndef NRF52_SERIES
    // Forecast broadcast at local 6am and 6pm
    if ((localHour == 6 || localHour == 18) && localHour != lastForecastHour_) {
        lastForecastHour_ = localHour;
        float fcLat = 0, fcLon = 0;
        const meshtastic_NodeInfoLite *bbs = nodeDB->getMeshNode(nodeDB->getNodeNum());
        if (bbs && bbs->has_position) {
            fcLat = bbs->position.latitude_i / 1e7f;
            fcLon = bbs->position.longitude_i / 1e7f;
        }
        fetchForecast(forecastCache_, sizeof(forecastCache_), fcLat, fcLon);
        lastForecastFetchTime_ = (uint32_t)getTime();
        if (forecastCache_[0] != '\0') {
            sendToPublicChannel(forecastCache_);
            LOG_DEBUG("[BBS] runOnce: sent forecast for local hour %u\n", localHour);
        }
    } else if (localHour != 6 && localHour != 18) {
        lastForecastHour_ = 255; // reset so next 6am/6pm fires
    }

    // Nightly 10pm local announcement
    if (localHour == 22 && lastNightlyHour_ != 22) {
        lastNightlyHour_ = 22;
        sendToPublicChannel("It's 10pm, do you know if your nodes are plugged in?");
    } else if (localHour != 22) {
        lastNightlyHour_ = 255;
    }
#else
    (void)localHour;
#endif

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
#ifdef NRF52_SERIES
    // Skip FSCom on nRF52 — InternalFS mutex contention causes crashes
    (void)nodeNum;
    return false;
#else
    File f = FSCom.open(SEEN_FILE, FILE_O_READ);
    if (!f) return false;
    uint32_t n;
    while (f.available() >= (int)sizeof(uint32_t)) {
        f.read((uint8_t *)&n, sizeof(uint32_t));
        if (n == nodeNum) { f.close(); return true; }
    }
    f.close();
    return false;
#endif
}

static void seenUserAdd(uint32_t nodeNum) {
#ifdef NRF52_SERIES
    // Skip FSCom on nRF52 — InternalFS mutex contention causes crashes
    (void)nodeNum;
#else
    File f = FSCom.open(SEEN_FILE, BBS_FILE_APPEND_MODE);
    if (!f) return;
    f.write((const uint8_t *)&nodeNum, sizeof(uint32_t));
    f.close();
#endif
}

ProcessMessage BBSModule::handleReceived(const meshtastic_MeshPacket &mp) {
#ifndef ARCH_ESP32
    // nRF52: lazy storage init — do it once on first message
    // Try external QSPI flash first (2MB), fall back to internal LittleFS (~28KB)
    if (!storage_) {
#ifdef NRF52_SERIES
#ifdef BBS_KB_LOADER
        {
            bool kbOk = kbLoaderRun();
            // Report what was written
            char kbMsg[180];
            uint32_t wSize = 0, gSize = 0;
            File wf = bbsExtFS().open("/bbs/kb/wordle.bin", FILE_O_READ);
            if (wf) { wSize = wf.size(); wf.close(); }
            File gf = bbsExtFS().open("/bbs/kb/geo_us.bin", FILE_O_READ);
            if (gf) { gSize = gf.size(); gf.close(); }
            snprintf(kbMsg, sizeof(kbMsg),
                     "KB: %s\nwordle:%u geo:%u",
                     kbOk ? "OK" : "FAIL",
                     wSize, gSize);
            sendReply(mp, kbMsg);
        }
#endif
        storage_ = new BBSStorageExtFlash();
        if (storage_ && !storage_->init()) {
            LOG_WARN("[BBS] External flash failed, falling back to internal FS\n");
            delete storage_;
            storage_ = nullptr;
        }
#endif
        if (!storage_) {
            storage_ = new BBSStorageLittleFS();
            if (storage_ && !storage_->init()) {
                delete storage_;
                storage_ = nullptr;
            }
        }
    }
#endif
    if (!storage_) return ProcessMessage::CONTINUE;

    // ACK the sender immediately, before any BBS processing or delay() calls.
    // This prevents our reply packets from blocking the ACK in the TX queue.
    if (mp.want_ack && routingModule) {
        routingModule->sendAckNak(meshtastic_Routing_Error_NONE, getFrom(&mp),
                                  mp.id, mp.channel);
    }

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

    LOG_DEBUG("[BBS] Rx from=0x%08x to=0x%08x: %s\n", mp.from, mp.to, buf);

    // Update UI frame tracking
    uiLastMsgTime_ = (uint32_t)getTime();
    const char *sn = getNodeShortName(mp.from);
    strncpy(uiLastMsgFrom_, sn ? sn : "????", sizeof(uiLastMsgFrom_) - 1);
    uiLastMsgFrom_[sizeof(uiLastMsgFrom_) - 1] = '\0';

#if defined(NRF52_SERIES) && defined(MESHTASTIC_EXCLUDE_SCREEN)
    tinyScreen_.notifyMessage(mp.from, sn, uiLastMsgTime_);
#endif

    bool isDM = (mp.to == nodeDB->getNodeNum()) && !isBroadcast(mp.to);

    if (isDM) {
        const char *text = buf;
#ifdef NRF52_SERIES
        // Knowledge base upload commands (!KB OPEN/DATA/CLOSE)
        if (strncasecmp(text, "!KB ", 4) == 0) {
            handleKBUpload(text + 4);
            return ProcessMessage::STOP;
        }
#endif
        if (strncasecmp(text, "!bbs", 4) == 0) {
            text += 4;
            while (*text == ' ' || *text == '\t') text++;
        }
        return handleDM(mp, text);
    } else if (isBroadcast(mp.to)) {
        // Tap-back for test/ack messages — reply with hop-count emoji
        if (mp.from != nodeDB->getNodeNum()) {
            // Word-boundary match for: test, testing, ack, acknowledge
            auto hasWord = [](const char *hay, const char *needle) -> bool {
                size_t nlen = strlen(needle);
                for (const char *p = hay; *p; p++) {
                    if (strncasecmp(p, needle, nlen) == 0) {
                        bool startOk = (p == hay) || !isalnum((unsigned char)p[-1]);
                        bool endOk   = !isalnum((unsigned char)p[nlen]);
                        if (startOk && endOk) return true;
                    }
                }
                return false;
            };
            // Tapback test/ack messages with hop-count emoji
            if (mp.decoded.reply_id == 0 &&
                (hasWord(buf, "test") || hasWord(buf, "testing") ||
                 hasWord(buf, "ack") || hasWord(buf, "acknowledge"))) {
                uint8_t hops = (mp.hop_start >= mp.hop_limit)
                               ? (mp.hop_start - mp.hop_limit) : 0;
                sendTapBack(mp, hops);
            }
            if (hasWord(buf, "ping")) {
                // 🏓 U+1F3D3 ping pong paddle
                static const char *pingEmoji = "\xF0\x9F\x8F\x93";
                meshtastic_MeshPacket *reply = allocDataPacket();
                if (reply) {
                    reply->to                    = NODENUM_BROADCAST;
                    reply->channel               = mp.channel;
                    reply->want_ack              = false;
                    reply->decoded.want_response = false;
                    reply->decoded.emoji         = 1;
                    reply->decoded.reply_id      = mp.id;
                    size_t elen = strlen(pingEmoji);
                    memcpy(reply->decoded.payload.bytes, pingEmoji, elen);
                    reply->decoded.payload.size = elen;
                    service->sendToMesh(reply);
                }
            }

            // 🤖 tapback if message looks like a flight number or mentions being on a plane
            {
                bool isFlightMsg = hasWord(buf, "on a plane") ||
                                   hasWord(buf, "on the plane") ||
                                   hasWord(buf, "im flying") ||
                                   hasWord(buf, "i'm flying") ||
                                   hasWord(buf, "inflight") ||
                                   hasWord(buf, "in-flight") ||
                                   hasWord(buf, "in flight");

                // Detect flight number pattern: 2-letter airline code + 1-4 digits (e.g. AA123, UA4567)
                if (!isFlightMsg) {
                    for (const char *p = buf; *p && !isFlightMsg; p++) {
                        if (isupper((unsigned char)p[0]) && isupper((unsigned char)p[1]) &&
                            isdigit((unsigned char)p[2])) {
                            // Check it's preceded by start or non-alpha
                            if (p == buf || !isalpha((unsigned char)p[-1])) {
                                int digits = 0;
                                for (int d = 2; d < 6 && isdigit((unsigned char)p[d]); d++) digits++;
                                // Check followed by non-alpha (end of word)
                                if (digits >= 1 && digits <= 4 &&
                                    !isalpha((unsigned char)p[2 + digits])) {
                                    isFlightMsg = true;
                                }
                            }
                        }
                    }
                }

                if (isFlightMsg) {
                    static const char *robotEmoji = "\xF0\x9F\xA4\x96"; // 🤖 U+1F916
                    meshtastic_MeshPacket *reply = allocDataPacket();
                    if (reply) {
                        reply->to                    = NODENUM_BROADCAST;
                        reply->channel               = mp.channel;
                        reply->want_ack              = false;
                        reply->decoded.want_response = false;
                        reply->decoded.emoji         = 1;
                        reply->decoded.reply_id      = mp.id;
                        size_t elen = strlen(robotEmoji);
                        memcpy(reply->decoded.payload.bytes, robotEmoji, elen);
                        reply->decoded.payload.size = elen;
                        service->sendToMesh(reply);
                    }
                }
            }
        }
        if (strncasecmp(buf, "!bbs", 4) != 0) return ProcessMessage::CONTINUE;
        const char *cmd = buf + 4;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        return handleChannelCmd(mp, cmd);
    } else {
        // Directed to someone else — not our packet
        return ProcessMessage::CONTINUE;
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
            LOG_DEBUG("[BBS] Session expired for 0x%08x\n", sessions_[i].nodeNum);
            memset(&sessions_[i], 0, sizeof(BBSSession));
        }
    }
}

// ─── DM handler ───────────────────────────────────────────────────────────

ProcessMessage BBSModule::handleDM(const meshtastic_MeshPacket &mp, const char *text) {
    if (isBroadcast(mp.to)) return ProcessMessage::CONTINUE; // channel messages never handled as DMs
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
#ifdef NRF52_SERIES
                sendReply(mp, "Welcome to TinyBBS nRf: A BBS running on an nRF52 node\nSend [H] for help menu.");
#else
                sendReply(mp, "Welcome to TinyBBS: VaultTec Ed.!\nSend [H] for help.");
#endif
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
        case BBS_STATE_GAMES:          return handleStateGames(mp, session, text);
        case BBS_STATE_WORDLE:         return handleStateWordle(mp, session, text);
        case BBS_STATE_VAULT:          return handleStateVault(mp, session, text);
        case BBS_STATE_WASTELAND:      return handleStateWasteland(mp, session, text);
        case BBS_STATE_CHESS:          return handleStateChess(mp, session, text);
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
#ifdef NRF52_SERIES
    if (unread > 0) {
        snprintf(menu, sizeof(menu),
                 "TinyBBS nRF52\n"
                 "[B]ulletins\n"
                 "[M]ail (%u unread)\n"
                 "[Q]SL\n"
                 "[G]ames\n"
                 "[S]tats\n"
                 "[X]Exit",
                 unread);
    } else {
        snprintf(menu, sizeof(menu),
                 "TinyBBS nRF52\n"
                 "[B]ulletins\n"
                 "[M]ail\n"
                 "[Q]SL\n"
                 "[G]ames\n"
                 "[S]tats\n"
                 "[X]Exit");
    }
#else
    if (unread > 0) {
        snprintf(menu, sizeof(menu),
                 "TinyBBS: VaultTec Ed.\n"
                 "[B]ulletins\n"
                 "[M]ail (%u unread)\n"
                 "[Q]SL\n"
                 "[G]ames [S]tats\n"
                 "[F]orecast [X]Exit",
                 unread);
    } else {
        snprintf(menu, sizeof(menu),
                 "TinyBBS: VaultTec Ed.\n"
                 "[B]ulletins\n"
                 "[M]ail\n"
                 "[Q]SL\n"
                 "[G]ames [S]tats\n"
                 "[F]orecast [X]Exit");
    }
#endif
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

void BBSModule::sendGamesMenu(const meshtastic_MeshPacket &req) {
    sendReply(req,
              "=== Games ===\n"
              "[W]ordle\n"
              "[V]ault-Tec Hack\n"
              "[R]PG Wasteland\n"
              "[C]hess by Mesh\n"
              "[X]Back");
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
        case 'g':
            session.state = BBS_STATE_GAMES;
            sendGamesMenu(mp);
            break;
#ifndef NRF52_SERIES
        case 'f':
            doForecast(mp);
            break;
#endif
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
                      "[B]ulletins\n"
                      "[M]ail\n"
                      "[Q]SL\n"
                      "[G]ames\n"
                      "[S]tats\n"
                      "[X]Exit");
            break;
        default:
            sendMainMenu(mp, session);
            break;
    }
    return ProcessMessage::STOP;
}

ProcessMessage BBSModule::handleStateGames(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendGamesMenu(mp); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);
    switch (cmd) {
        case 'w':
            doWordleStart(mp, session);
            break;
        case 'v':
            doVaultStart(mp, session);
            break;
        case 'r':
            session.state = BBS_STATE_WASTELAND;
            {
                char rpgBuf[512];
                bool exitGame = false;
                frpgCommand(mp.from, "", getNodeShortName(mp.from), rpgBuf, sizeof(rpgBuf), exitGame);
                sendReplyMultipart(mp, rpgBuf);
                delay(MULTIPART_DELAY_MS);
                sendReply(mp,
                    "EX-Explore  SH-Shop\n"
                    "DR-Heal     TR-Train\n"
                    "AR-Arena    TV-Tavern\n"
                    "NV-Casino   ST-Stats\n"
                    "CH-Cheng    LB-Board\n"
                    "AB-About    HELP\n"
                    "[X]Back to BBS");
            }
            break;
        case 'c':
            chessEnsureDir();
            session.state = BBS_STATE_CHESS;
            session.chessGameId = 0;
            sendChessStatus(mp, session);
            break;
        case 'x':
            session.state = BBS_STATE_MAIN;
            sendMainMenu(mp, session);

            break;
        default:
            sendGamesMenu(mp);
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
    // 9am local time boundary; utcOffsetSeconds_ is negative for US timezones
    // e.g. EDT=-14400: 9am Eastern = 13:00 UTC, so (t - 14400 - 9*3600)/86400 rolls at right time
    // Falls back to 9am UTC if offset not yet known (before first weather fetch)
    int64_t adj = (int64_t)t + utcOffsetSeconds_ - 9 * 3600;
    if (adj < 0) return 0;
    return (uint32_t)(adj / 86400);
}

#ifndef NRF52_SERIES
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

#endif // NRF52_SERIES (Wordle score persistence)

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

    // Check if already played today
#ifndef NRF52_SERIES
    if (wordleHasPlayed(day, req.from)) {
        char standings[180];
        buildStandings(day, standings, sizeof(standings));
        char msg[200];
        snprintf(msg, sizeof(msg), "Already played today!\n%s", standings);
        sendReply(req, msg);
        return; // stay in BBS_STATE_MAIN
    }
#endif // NRF52_SERIES

    // Pick today's daily word — try external 12K dictionary, fall back to embedded 160
#ifdef NRF52_SERIES
    if (!wordlePickWordExt(day, session.wordleTarget)) {
#endif
        const char *word = wordlePickWord(day);
        strncpy(session.wordleTarget, word, 5);
        session.wordleTarget[5] = '\0';
#ifdef NRF52_SERIES
    }
#endif
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

    // Validate against word list
    {
        bool valid = false;
#ifdef NRF52_SERIES
        // Try external 12K dictionary on flash, fall back to accept-all
        if (wordleExtDictAvailable())
            valid = wordleIsValidExt(guess);
        else
            valid = wordleIsValid(guess); // accepts any 5-letter alpha
#else
        valid = wordleIsValid(guess); // ESP32: binary search embedded 12K list
#endif
        if (!valid) {
            sendReply(mp, "Not a valid word. Try again.");
            return ProcessMessage::STOP;
        }
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
#ifndef NRF52_SERIES
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
#else
        (void)guessCount;
#endif
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
                  "Tiny BBS VaultTec Ed.: DM this node, H for help menu\n"
                  "!bbs qsl, !bbs post <txt>\n"
                  "!bbs list, !bbs stats\n"
                  "!bbs wx");
        return ProcessMessage::STOP;
    }
    if (strncasecmp(cmd, "qsl", 3) == 0) {
        doQSLPost(mp);
        // Also announce to public channel
        BBSQSL lastQsl;
        uint32_t totalQsl = storage_->totalActiveQSL();
        if (totalQsl > 0) {
            BBSQSLHeader qh;
            if (storage_->listQSL(&qh, 1, 0) > 0) {
                uint8_t hops = qh.hopsAway;
                char announce[160];
                if (qh.location[0])
                    snprintf(announce, sizeof(announce),
                             "QSL: %s (%s) %u hop(s)", qh.fromName, qh.location, hops);
                else
                    snprintf(announce, sizeof(announce),
                             "QSL: %s %u hop(s)", qh.fromName, hops);
                sendToPublicChannel(announce);
            }
        }
    } else
    if (strncasecmp(cmd, "post", 4) == 0) {
        const char *text = cmd + 4;
        while (*text == ' ' || *text == '\t') text++;
        doBulletinPost(mp, text, BOARD_GENERAL);
    } else if (strncasecmp(cmd, "list", 4) == 0) {
        doBulletinList(mp, 1, BOARD_ALL);
    } else if (strncasecmp(cmd, "stats", 5) == 0) {
        doStats(mp);
#ifndef NRF52_SERIES
    } else if (strncasecmp(cmd, "wx", 2) == 0 || strncasecmp(cmd, "forecast", 8) == 0) {
        doForecast(mp);
#endif
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
        if (headers[i].location[0])
            snprintf(line, sizeof(line), "#%u %s %s %uhops\n",
                     headers[i].id, headers[i].fromName, headers[i].location, headers[i].hopsAway);
        else
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
    qsl.location[0] = '\0';

    const char *name = getNodeShortName(req.from);
    if (name) strncpy(qsl.fromName, name, BBS_SHORT_NAME_LEN - 1);
    else snprintf(qsl.fromName, BBS_SHORT_NAME_LEN, "!%08x", req.from);
    qsl.fromName[BBS_SHORT_NAME_LEN - 1] = '\0';

    // Look up sender's GPS position and reverse geocode to city name
    float lat = 0, lon = 0;
    const meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(req.from);
    if (sender && sender->has_position &&
        !(sender->position.latitude_i == 0 && sender->position.longitude_i == 0)) {
        lat = sender->position.latitude_i / 1e7f;
        lon = sender->position.longitude_i / 1e7f;
        qsl.latitude  = sender->position.latitude_i;
        qsl.longitude = sender->position.longitude_i;
        qsl.altitude  = sender->position.altitude;
    }
    if (lat != 0.0f || lon != 0.0f) {
        // Try external flash (17K cities), then embedded (500 cities), then WiFi
        if (!geoLookup(lat, lon, qsl.location, sizeof(qsl.location))) {
#ifndef NRF52_SERIES
            reverseGeocode(lat, lon, qsl.location, sizeof(qsl.location));
#endif
        }
    }

    if (storage_->storeQSL(qsl)) {
        // Format timestamp as MM/DD HH:MM
        uint32_t ts = qsl.timestamp;
        time_t t = (time_t)ts;
        struct tm *tm = gmtime(&t);
        char dateStr[8];
        strftime(dateStr, sizeof(dateStr), "%m/%d", tm);

        char reply[160];
        if (qsl.location[0])
            snprintf(reply, sizeof(reply), "QSL #%u: %s (%s)\n%uhop(s) %s",
                     qsl.id, qsl.fromName, qsl.location, qsl.hopsAway, dateStr);
        else
            snprintf(reply, sizeof(reply), "QSL #%u: %s\n%uhop(s) %s",
                     qsl.id, qsl.fromName, qsl.hopsAway, dateStr);
        if (isBroadcast(req.to)) sendToChannel(req, reply);
        else sendReply(req, reply);
    } else {
        sendReply(req, "Failed to post QSL.");
    }
}

void BBSModule::doStats(const meshtastic_MeshPacket &req) {
    BBSStats stats = storage_->getStats();
    char reply[220];
    const char *backend = "LittleFS";
#ifdef NRF52_SERIES
    if (stats.freeBytesEstimate > 0) backend = "ExtFlash";
#endif
#ifdef ARCH_ESP32
    if (ESP.getFreePsram() > 1024 * 1024) backend = "PSRAM";
#endif
    snprintf(reply, sizeof(reply),
             "BBS Stats [%s]:\n"
             "Bulletins: %u/%u\n"
             "Mail: %u items\n"
             "QSL: %u posts\n"
             "Free: %luKB",
             backend,
             stats.totalBulletins, stats.maxBulletins,
             stats.totalMailItems, stats.totalQSLItems,
             (unsigned long)(stats.freeBytesEstimate / 1024));
    sendReply(req, reply);
}

// ─── Forecast ─────────────────────────────────────────────────────────────
#ifndef NRF52_SERIES

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

// Get Nth value from a JSON array like [41.2,45.0,38.5]
static float jsonArrayNth(const char *tag, int n) {
    if (!tag) return -999.0f;
    const char *p = strchr(tag, '[');
    if (!p) return -999.0f;
    p++;
    for (int i = 0; i < n; i++) {
        while (*p && *p != ',' && *p != ']') p++;
        if (*p != ',') return -999.0f;
        p++;
    }
    return atof(p);
}

// WMO weather code to short description
static const char *wmoToText(int code) {
    if (code == 0)  return "Clear";
    if (code <= 2)  return "Partly Cloudy";
    if (code <= 3)  return "Overcast";
    if (code <= 48) return "Foggy";
    if (code <= 55) return "Drizzle";
    if (code <= 65) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Showers";
    if (code <= 86) return "Snow Showers";
    return "Stormy";
}

// WMO weather code to emoji (UTF-8, BMP only — 3-byte max)
static const char *wmoToEmoji(int code) {
    if (code == 0)  return "\xE2\x98\x80";  // ☀
    if (code <= 2)  return "\xE2\x9B\x85";  // ⛅
    if (code <= 3)  return "\xE2\x98\x81";  // ☁
    if (code <= 48) return "\xE2\x98\x81";  // ☁ (fog → cloudy)
    if (code <= 55) return "\xE2\x98\x94";  // ☔
    if (code <= 65) return "\xE2\x98\x94";  // ☔
    if (code <= 77) return "\xE2\x9D\x84";  // ❄
    if (code <= 82) return "\xE2\x98\x94";  // ☔
    if (code <= 86) return "\xE2\x9D\x84";  // ❄
    return "\xE2\x9B\x88";                  // ⛈
}

bool BBSModule::reverseGeocode(float lat, float lon, char *city, size_t cityLen) {
#ifndef ARCH_ESP32
    (void)lat; (void)lon; (void)city; (void)cityLen;
    return false;
#else
    city[0] = '\0';
    static char gbuf[512];
    char gurl[256];
    snprintf(gurl, sizeof(gurl),
             "https://nominatim.openstreetmap.org/reverse"
             "?lat=%.4f&lon=%.4f&format=json&zoom=10&addressdetails=1",
             lat, lon);
    WiFiClientSecure gwc;
    gwc.setInsecure();
    HTTPClient ghttp;
    ghttp.setTimeout(8000);
    ghttp.begin(gwc, gurl);
    ghttp.addHeader("User-Agent", "MeshBBS/1.0");
    int gcode = ghttp.GET();
    LOG_DEBUG("[GEO] nominatim code=%d\n", gcode);
    if (gcode == 200) {
        String gbody = ghttp.getString();
        strncpy(gbuf, gbody.c_str(), sizeof(gbuf) - 1);
        gbuf[sizeof(gbuf) - 1] = '\0';
        const char *keys[] = {"\"city\":\"", "\"town\":\"", "\"village\":\"", "\"county\":\""};
        for (const char *key : keys) {
            const char *cp = strstr(gbuf, key);
            if (cp) {
                cp += strlen(key);
                const char *ce = strchr(cp, '"');
                if (ce) {
                    size_t len = ce - cp;
                    if (len >= cityLen) len = cityLen - 1;
                    memcpy(city, cp, len);
                    city[len] = '\0';
                    break;
                }
            }
        }
    }
    ghttp.end();
    gwc.stop();
    return city[0] != '\0';
#endif
}

bool BBSModule::fetchForecast(char *buf, size_t bufLen, float lat, float lon) {
#ifndef ARCH_ESP32
    snprintf(buf, bufLen, "Forecast not supported on this platform.");
    return false;
#else
    LOG_DEBUG("[FC] WiFi.status()=%d\n", (int)WiFi.status());

    if (lat == 0.0f && lon == 0.0f) {
        snprintf(buf, bufLen, "No GPS fix - forecast unavailable.");
        return false;
    }

    // Single HTTP (no SSL) request to Open-Meteo — fast, free, no key needed
    static char sbuf[2048];
    char url[256];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weathercode"
             "&temperature_unit=fahrenheit&timezone=auto&forecast_days=6",
             lat, lon);

    LOG_DEBUG("[FC] heap=%u url=%s\n", (unsigned)ESP.getFreeHeap(), url);

    // Try up to 2 times — first attempt may fail if previous connection was stale
    String body;
    int code = -1;
    for (int attempt = 0; attempt < 2 && code != 200; attempt++) {
        if (attempt > 0) delay(500);
        WiFiClient wc;
        HTTPClient http;
        http.setTimeout(15000);
        http.begin(wc, url);
        code = http.GET();
        LOG_DEBUG("[FC] attempt=%d code=%d\n", attempt, code);
        if (code == 200) {
            body = http.getString();
        }
        http.end();
        wc.stop();
    }

    if (code != 200) {
        snprintf(buf, bufLen, "Forecast error: %d", code);
        return false;
    }

    strncpy(sbuf, body.c_str(), sizeof(sbuf) - 1);
    sbuf[sizeof(sbuf) - 1] = '\0';

    LOG_DEBUG("[FC] body=%u bytes\n", (unsigned)body.length());

    // Parse Open-Meteo JSON — arrays have 3 values (one per day)
    const char *tmax = strstr(sbuf, "\"temperature_2m_max\":[");
    const char *tmin = strstr(sbuf, "\"temperature_2m_min\":[");
    const char *prec = strstr(sbuf, "\"precipitation_probability_max\":[");
    const char *wcod = strstr(sbuf, "\"weathercode\":[");

    if (!tmax || !tmin) {
        snprintf(buf, bufLen, "No forecast data.");
        return false;
    }

    // Day labels: parse "time":["YYYY-MM-DD",...] from response
    // Fall back to Today/+1/+2 from RTC if parsing fails
    static const char *dow[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char *labels[6] = {"Today","Day2","Day3","Day4","Day5","Day6"};
    {
        auto dateToDow = [](const char *d) -> int {
            int y = atoi(d), m = atoi(d + 5), day = atoi(d + 8);
            if (m < 3) { m += 12; y--; }
            int k = y % 100, j = y / 100;
            int h = (day + (13*(m+1))/5 + k + k/4 + j/4 + 5*j) % 7;
            return (h + 6) % 7;
        };
        const char *tp = strstr(sbuf, "\"time\":[\"");
        if (tp) {
            tp += 9;
            for (int i = 0; i < 6; i++) {
                if (tp[0] && tp[1] && tp[2] && tp[3] && tp[4] && tp[5] && tp[6] && tp[7] && tp[8] && tp[9]) {
                    int d = dateToDow(tp);
                    labels[i] = (i == 0) ? "Today" : dow[d];
                }
                while (*tp && *tp != '"') tp++;
                if (*tp == '"') tp++;
                if (*tp == ',') tp++;
                if (*tp == '"') tp++;
            }
        }
    }

    // Cache UTC offset for scheduled broadcasts
    const char *utcOff = strstr(sbuf, "\"utc_offset_seconds\":");
    if (utcOff) utcOffsetSeconds_ = atoi(utcOff + 21);

    // Reverse geocode lat/lon to get city name via Nominatim (HTTPS)
    char city[48] = {0};
    reverseGeocode(lat, lon, city, sizeof(city));

    size_t pos = 0;
    if (city[0])
        pos += snprintf(buf + pos, bufLen - pos, "TinyBBS 5Day 4Cast (%s):\n", city);
    else
        pos += snprintf(buf + pos, bufLen - pos, "TinyBBS 5Day 4Cast:\n");
    for (int i = 0; i < 6 && pos < bufLen - 1; i++) {
        float hi = jsonArrayNth(tmax, i);
        float lo = jsonArrayNth(tmin, i);
        int pp   = prec ? (int)(jsonArrayNth(prec, i) + 0.5f) : 0;
        int wc   = wcod ? (int)(jsonArrayNth(wcod, i) + 0.5f) : 0;
        const char *em = wmoToEmoji(wc);
        if (i == 0) {
            pos += snprintf(buf + pos, bufLen - pos,
                            "Today L/H %d\xC2\xB0/%d\xC2\xB0 Precip %d%% Cond %s\n",
                            (int)(lo + 0.5f), (int)(hi + 0.5f), pp, em);
        } else {
            pos += snprintf(buf + pos, bufLen - pos, "%s %d\xC2\xB0/%d\xC2\xB0 %d%%%s\n",
                            labels[i],
                            (int)(lo + 0.5f), (int)(hi + 0.5f), pp, em);
        }
    }
    if (pos > 0 && buf[pos - 1] == '\n') buf[--pos] = '\0';

    return true;
#endif
}

void BBSModule::doForecast(const meshtastic_MeshPacket &req) {
    uint32_t now = (uint32_t)getTime();

    // Resolve requester position first so we can check location drift
    float lat = 0, lon = 0;
    {
        bool gotPos = false;
        const meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(req.from);
        if (sender && sender->has_position &&
            !(sender->position.latitude_i == 0 && sender->position.longitude_i == 0)) {
            lat = sender->position.latitude_i / 1e7f;
            lon = sender->position.longitude_i / 1e7f;
            gotPos = true;
        }
        if (!gotPos && req.relay_node != 0) {
            for (size_t i = 0; i < nodeDB->getNumMeshNodes() && !gotPos; i++) {
                const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                if (!n || !n->has_position) continue;
                if ((n->num & 0xFF) != req.relay_node) continue;
                if (n->position.latitude_i == 0 && n->position.longitude_i == 0) continue;
                lat = n->position.latitude_i / 1e7f;
                lon = n->position.longitude_i / 1e7f;
                gotPos = true;
            }
        }
        if (!gotPos) {
            uint8_t bestHops = 255;
            for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                if (!n || !n->has_position) continue;
                if (n->num == nodeDB->getNodeNum()) continue;
                if (n->position.latitude_i == 0 && n->position.longitude_i == 0) continue;
                if (n->hops_away < bestHops) {
                    bestHops = n->hops_away;
                    lat = n->position.latitude_i / 1e7f;
                    lon = n->position.longitude_i / 1e7f;
                    gotPos = true;
                }
            }
        }
        if (!gotPos) {
            const meshtastic_NodeInfoLite *bbs = nodeDB->getMeshNode(nodeDB->getNodeNum());
            if (bbs && bbs->has_position &&
                !(bbs->position.latitude_i == 0 && bbs->position.longitude_i == 0)) {
                lat = bbs->position.latitude_i / 1e7f;
                lon = bbs->position.longitude_i / 1e7f;
            }
        }
    }

    bool cacheValid = (forecastCache_[0] != '\0') &&
                      (now > lastForecastFetchTime_) &&
                      ((now - lastForecastFetchTime_) < FORECAST_CACHE_TTL_S);

    if (!cacheValid) {
        // Always fetch with requester's current location
        if (lat != 0.0f || lon != 0.0f)
            fetchForecast(forecastCache_, sizeof(forecastCache_), lat, lon);
        lastForecastFetchTime_ = now;
    }

    const char *msg = forecastCache_[0] ? forecastCache_ : "Forecast unavailable.";
    sendReply(req, msg);
}

#endif // NRF52_SERIES (forecast)

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

void BBSModule::sendTapBack(const meshtastic_MeshPacket &req, uint8_t hops) {
    // Number keycap emojis: digit + U+FE0F (var selector) + U+20E3 (combining keycap)
    // All BMP characters — 1 + 3 + 3 = 7 bytes each, valid UTF-8
    static const char *hopEmoji[8] = {
        "0\xEF\xB8\x8F\xE2\x83\xA3",   // 0️⃣
        "1\xEF\xB8\x8F\xE2\x83\xA3",   // 1️⃣
        "2\xEF\xB8\x8F\xE2\x83\xA3",   // 2️⃣
        "3\xEF\xB8\x8F\xE2\x83\xA3",   // 3️⃣
        "4\xEF\xB8\x8F\xE2\x83\xA3",   // 4️⃣
        "5\xEF\xB8\x8F\xE2\x83\xA3",   // 5️⃣
        "6\xEF\xB8\x8F\xE2\x83\xA3",   // 6️⃣
        "7\xEF\xB8\x8F\xE2\x83\xA3",   // 7️⃣
    };
    if (hops > 7) hops = 7;
    const char *emoji = hopEmoji[hops];

    meshtastic_MeshPacket *reply = allocDataPacket();
    if (!reply) return;
    reply->to           = NODENUM_BROADCAST;
    reply->channel      = req.channel;
    reply->want_ack     = false;
    reply->decoded.want_response = false;
    reply->decoded.emoji    = 1;        // marks this as a reaction/tap-back
    reply->decoded.reply_id = req.id;   // react to the original message
    size_t len = strlen(emoji);
    reply->decoded.payload.size = (uint32_t)len;
    memcpy(reply->decoded.payload.bytes, emoji, len);
    service->sendToMesh(reply);
    LOG_DEBUG("[BBS] tap-back %d hops for msg id=0x%08x\n", hops, req.id);
}

bool BBSModule::sendReply(const meshtastic_MeshPacket &req, const char *text) {
    if (!text) return false;
    // Channel commands reply to the channel; DMs reply to the sender
    if (isBroadcast(req.to)) return sendToChannel(req, text);
    meshtastic_MeshPacket *reply = allocDataPacket();
    LOG_DEBUG("[BBS] sendReply: allocDataPacket=%p text='%.40s'\n", (void*)reply, text);
    if (!reply) return false;
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

// ─── Vault-Tec Hacking ────────────────────────────────────────────────────

static uint8_t vaultPositionalMatches(const char *a, const char *b) {
    uint8_t n = 0;
    for (int i = 0; i < 5; i++)
        if (tolower((unsigned char)a[i]) == tolower((unsigned char)b[i])) n++;
    return n;
}

void BBSModule::doVaultStart(const meshtastic_MeshPacket &req, BBSSession &session) {
    static const uint32_t TOTAL = sizeof(WORDLE_WORDS) / sizeof(WORDLE_WORDS[0]);

    // Pick a random answer
    uint32_t answerIdx = random(TOTAL);
    const char *answer = WORDLE_WORDS[answerIdx];

    // Reservoir-sample 11 decoys with 2-3 positional matches (single pass)
    uint16_t picked[11];
    uint32_t pickCount = 0;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < TOTAL; i++) {
        if (i == answerIdx) continue;
        uint8_t m = vaultPositionalMatches(answer, WORDLE_WORDS[i]);
        if (m < 2 || m > 3) continue;
        seen++;
        if (pickCount < 11) {
            picked[pickCount++] = (uint16_t)i;
        } else {
            uint32_t j = random(seen);
            if (j < 11) picked[j] = (uint16_t)i;
        }
    }

    // Fallback: relax to 1-4 matches if not enough
    if (pickCount < 11) {
        seen = 0; pickCount = 0;
        for (uint32_t i = 0; i < TOTAL; i++) {
            if (i == answerIdx) continue;
            uint8_t m = vaultPositionalMatches(answer, WORDLE_WORDS[i]);
            if (m < 1 || m > 4) continue;
            seen++;
            if (pickCount < 11) {
                picked[pickCount++] = (uint16_t)i;
            } else {
                uint32_t j = random(seen);
                if (j < 11) picked[j] = (uint16_t)i;
            }
        }
    }

    // Place answer at a random slot; fill rest with decoys
    uint8_t answerSlot = (uint8_t)random(12);
    uint8_t ci = 0;
    for (int i = 0; i < 12; i++) {
        if (i == answerSlot) {
            strncpy(session.vaultWords[i], answer, 5);
        } else if (ci < pickCount) {
            strncpy(session.vaultWords[i], WORDLE_WORDS[picked[ci++]], 5);
        } else {
            // shouldn't happen, but fill with random word
            strncpy(session.vaultWords[i], WORDLE_WORDS[random(TOTAL)], 5);
        }
        session.vaultWords[i][5] = '\0';
    }

    session.vaultAnswer  = answerSlot;
    session.vaultGuesses = 5;
    session.state        = BBS_STATE_VAULT;

    sendVaultBoard(req, session);
}

void BBSModule::sendVaultBoard(const meshtastic_MeshPacket &req, const BBSSession &session) {
    char buf[200];
    size_t pos = snprintf(buf, sizeof(buf), "=== VAULT-TEC HACK ===\n");
    for (int row = 0; row < 6 && pos + 24 < sizeof(buf); row++) {
        int a = row, b = row + 6;
        char wa[6], wb[6];
        for (int k = 0; k < 5; k++) {
            wa[k] = (char)toupper((unsigned char)session.vaultWords[a][k]);
            wb[k] = (char)toupper((unsigned char)session.vaultWords[b][k]);
        }
        wa[5] = wb[5] = '\0';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%2d.%s %2d.%s\n", a + 1, wa, b + 1, wb);
    }
    snprintf(buf + pos, sizeof(buf) - pos,
             "%u tries. Pick 1-12 or X:", session.vaultGuesses);
    sendReply(req, buf);
}

ProcessMessage BBSModule::handleStateVault(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') {
        sendVaultBoard(mp, session);
        return ProcessMessage::STOP;
    }

    char cmd0 = tolower((unsigned char)text[0]);
    if (cmd0 == 'x') {
        session.state = BBS_STATE_MAIN;
        sendMainMenu(mp, session);
        return ProcessMessage::STOP;
    }

    int pick = atoi(text);
    if (pick < 1 || pick > 12) {
        sendReply(mp, "Enter 1-12 or X to quit.");
        return ProcessMessage::STOP;
    }

    uint8_t idx = (uint8_t)(pick - 1);

    // Build uppercase version of picked word for feedback
    char wpicked[6];
    for (int k = 0; k < 5; k++) wpicked[k] = (char)toupper((unsigned char)session.vaultWords[idx][k]);
    wpicked[5] = '\0';

    if (idx == session.vaultAnswer) {
        char msg[60];
        snprintf(msg, sizeof(msg), "ACCESS GRANTED!\nPassword: %s", wpicked);
        sendReply(mp, msg);
        session.state = BBS_STATE_MAIN;
        delay(MULTIPART_DELAY_MS);
        sendMainMenu(mp, session);
        return ProcessMessage::STOP;
    }

    uint8_t matches = vaultPositionalMatches(session.vaultWords[session.vaultAnswer],
                                             session.vaultWords[idx]);
    session.vaultGuesses--;

    if (session.vaultGuesses == 0) {
        char wans[6];
        for (int k = 0; k < 5; k++) wans[k] = (char)toupper((unsigned char)session.vaultWords[session.vaultAnswer][k]);
        wans[5] = '\0';
        char msg[60];
        snprintf(msg, sizeof(msg), "%s > Likeness: %u/5\nLOCKED OUT.\nWas: %s", wpicked, matches, wans);
        sendReply(mp, msg);
        session.state = BBS_STATE_MAIN;
        delay(MULTIPART_DELAY_MS);
        sendMainMenu(mp, session);
        return ProcessMessage::STOP;
    }

    char msg[60];
    snprintf(msg, sizeof(msg), "%s > Likeness: %u/5\n%u tries left.", wpicked, matches, session.vaultGuesses);
    sendReply(mp, msg);
    delay(MULTIPART_DELAY_MS);
    sendVaultBoard(mp, session);
    return ProcessMessage::STOP;
}


// ─── Wasteland RPG door game ──────────────────────────────────────────────────

ProcessMessage BBSModule::handleStateWasteland(const meshtastic_MeshPacket &mp,
                                               BBSSession &session,
                                               const char *text) {
    // Intercept NV (New Vegas casino) before passing to RPG engine
    {
        char ucmd[8] = {0};
        strncpy(ucmd, text, 7);
        for (char *p = ucmd; *p; p++) *p = toupper((unsigned char)*p);
    }

    char rpgBuf[512];
    bool exitGame = false;
    frpgCommand(mp.from, text, getNodeShortName(mp.from), rpgBuf, sizeof(rpgBuf), exitGame);

    // Broadcast public channel announcement (e.g. Cheng kill)
    if (frpgPendingAnnounce) {
        frpgPendingAnnounce = false;
        sendToPublicChannel(frpgAnnounceMsg);
    }

    if (exitGame) {
        session.state = BBS_STATE_MAIN;
        sendReplyMultipart(mp, rpgBuf);
        delay(MULTIPART_DELAY_MS);
        sendMainMenu(mp, session);
    } else {
        sendReplyMultipart(mp, rpgBuf);
    }
    return ProcessMessage::STOP;
}


// ─── Chess by Mail ─────────────────────────────────────────────────────────

static void chessMoveDescribe(const ChessBoard boardBefore, const char *moveStr,
                              bool isPlayer, char *out, size_t outLen) {
    static const char *pieceName[] = {"","Pawn","Knight","Bishop","Rook","Queen","King"};
    static const char *captureFlavorPlayer[] = {
        "Nice take!", "Captured!", "Got 'em!", "Taken!", "Good grab!"
    };
    static const char *captureFlavorAI[] = {
        "Ouch!", "The AI strikes!", "Gotcha!", "Taken!", "That stings!"
    };

    int fr, ff, tr, tf;
    int8_t promo = 0;
    if (!chessParseMove(moveStr, &fr, &ff, &tr, &tf, &promo)) {
        snprintf(out, outLen, "%s", moveStr);
        return;
    }

    int8_t moving   = boardBefore[fr][ff];
    int8_t captured = boardBefore[tr][tf];
    int mIdx = moving   < 0 ? -moving   : moving;
    int cIdx = captured < 0 ? -captured : captured;

    // En passant: pawn moves diagonally to empty square
    bool isEnPassant = (mIdx == 1 && ff != tf && captured == 0);
    if (isEnPassant) cIdx = 1; // captures a pawn

    const char *mname = (mIdx >= 1 && mIdx <= 6) ? pieceName[mIdx] : "Piece";
    char toSq[3] = { (char)('a' + tf), (char)('1' + tr), 0 };
    char fromSq[3] = { (char)('a' + ff), (char)('1' + fr), 0 };

    if (cIdx > 0) {
        const char *cname = (cIdx >= 1 && cIdx <= 6) ? pieceName[cIdx] : "piece";
        snprintf(out, outLen, "%s %s takes %s at %s (%s)",
                 isPlayer ? "Your" : "AI",
                 mname, cname, toSq, moveStr);
    } else {
        snprintf(out, outLen, "%s %s %s->%s%s (%s)",
                 isPlayer ? "Your" : "AI",
                 mname, fromSq, toSq,
                 promo ? " promoted!" : "",
                 moveStr);
    }
}

static const char *chessStatusName[] = {"Easy AI", "Medium AI", "Hard AI"};
static const char *chessSideName[]   = {"White", "Black"};

void BBSModule::sendChessStatus(const meshtastic_MeshPacket &req, BBSSession &session) {
    chessEnsureDir();

    // If no game selected, list active games or show start prompt
    if (session.chessGameId == 0) {
        uint32_t ids[8];
        uint32_t count = chessListGames(req.from, ids, 8);
        if (count == 0) {
            sendReply(req,
                "Chess by Mail\n"
                "No active games.\n"
                "NEW - vs AI (easy)\n"
                "NEW2/NEW3 - med/hard\n"
                "NEW <name> - vs player\n"
                "[X]Back");
        } else {
            char buf[200] = "Chess games:\n";
            for (uint32_t i = 0; i < count && i < 5; i++) {
                BBSChessGame g;
                if (chessLoadGame(ids[i], g)) {
                    bool iAmWhite = (g.whiteNode == req.from);
                    bool myTurn = (iAmWhite && g.toMove == 0) || (!iAmWhite && g.toMove == 1);
                    const char *opp = (g.whiteNode == 0 || g.blackNode == 0)
                        ? chessStatusName[g.difficulty]
                        : getNodeShortName(iAmWhite ? g.blackNode : g.whiteNode);
                    char line[50];
                    snprintf(line, sizeof(line), "#%u vs %s%s\n",
                             ids[i], opp ? opp : "?", myTurn ? " *YOUR TURN*" : "");
                    strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
                }
            }
            strncat(buf, "#<id> to select\nNEW for new game\n[X]Back",
                    sizeof(buf) - strlen(buf) - 1);
            sendReply(req, buf);
        }
        return;
    }

    // Show current game status + send FEN as separate message
    BBSChessGame g;
    if (!chessLoadGame(session.chessGameId, g)) {
        session.chessGameId = 0;
        sendReply(req, "Game not found.\nType LG to list games.");
        return;
    }

    bool iAmWhite = (g.whiteNode == req.from);
    bool myTurn   = (iAmWhite && g.toMove == 0) || (!iAmWhite && g.toMove == 1);
    bool isAI     = (g.whiteNode == 0 || g.blackNode == 0);
    const char *opp = isAI
        ? chessStatusName[g.difficulty]
        : getNodeShortName(iAmWhite ? g.blackNode : g.whiteNode);

    static const char *statusStr[] = {"Active","White won","Black won","Draw","Stalemate"};
    char buf[200];
    if (g.status != 0) {
        snprintf(buf, sizeof(buf), "Game #%u vs %s\n%s\nMoves: %u",
                 g.id, opp ? opp : "?", statusStr[g.status], g.fullMoveNumber);
    } else {
        snprintf(buf, sizeof(buf),
                 "Game #%u vs %s\n"
                 "You are %s\n"
                 "%s to move (move %u)\n"
                 "Last: %s\n"
                 "%s",
                 g.id, opp ? opp : "?",
                 chessSideName[iAmWhite ? 0 : 1],
                 chessSideName[g.toMove],
                 g.fullMoveNumber,
                 g.lastMoveStr[0] ? g.lastMoveStr : "none",
                 myTurn ? "Your turn! Type move (e.g. e2e4)" : "Waiting for opponent.");
    }
    sendReply(req, buf);

    // Send FEN as a separate message for easy copy-paste
    if (g.status == 0 || true) {
        delay(MULTIPART_DELAY_MS);
        char fen[100];
        chessBuildFEN(g, fen, sizeof(fen));
        sendReply(req, fen);
    }
}

ProcessMessage BBSModule::handleStateChess(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendChessStatus(mp, session); return ProcessMessage::STOP; }

    // Uppercase copy for command matching
    char cmd[32] = {0};
    strncpy(cmd, text, sizeof(cmd) - 1);
    for (int i = 0; cmd[i]; i++) cmd[i] = toupper((unsigned char)cmd[i]);

    // [X] back to main
    if (cmd[0] == 'X' && cmd[1] == '\0') {
        session.state = BBS_STATE_MAIN;
        sendMainMenu(mp, session);
        return ProcessMessage::STOP;
    }

    // LG — list games
    if (strcmp(cmd, "LG") == 0) {
        session.chessGameId = 0;
        sendChessStatus(mp, session);
        return ProcessMessage::STOP;
    }

    // #<id> — select game
    if (cmd[0] == '#') {
        uint32_t id = (uint32_t)atoi(cmd + 1);
        BBSChessGame g;
        if (id > 0 && chessLoadGame(id, g) &&
            (g.whiteNode == mp.from || g.blackNode == mp.from)) {
            session.chessGameId = id;
            // If it's an AI game with the AI's turn pending, run the AI now
            if (g.status == 0 && g.blackNode == 0 && g.toMove == 1) {
                ChessBoard boardBeforeAI;
                memcpy(boardBeforeAI, g.board, sizeof(ChessBoard));
                char aiMoveStr[6] = {0};
                bool aiMoved = chessAIMove(g, aiMoveStr);
                g.status = chessCheckTermination(g);
                chessSaveGame(g);
                if (aiMoved) {
                    char aiDesc[80];
                    chessMoveDescribe(boardBeforeAI, aiMoveStr, false, aiDesc, sizeof(aiDesc));
                    bool nextCheck = (g.status == 0) && chessIsInCheck(g.board, g.toMove == 0);
                    char reply[200];
                    snprintf(reply, sizeof(reply), "AI move:\n%s\n%s", aiDesc,
                             g.status != 0 ? (g.status <= 2 ? (g.status == 1 ? "Checkmate! White wins!" : "Checkmate! Black wins!") : "Game over.")
                                           : (nextCheck ? "White to move. Check!" : "White to move."));
                    sendReply(mp, reply);
                    delay(MULTIPART_DELAY_MS);
                }
                if (g.status != 0) {
                    chessUpdateRatings(g.whiteNode, g.blackNode, g.status, g.difficulty);
                    char fen[100]; chessBuildFEN(g, fen, sizeof(fen));
                    sendReply(mp, fen);
                    return ProcessMessage::STOP;
                }
            }
        } else {
            sendReply(mp, "Game not found or not yours.");
        }
        sendChessStatus(mp, session);
        return ProcessMessage::STOP;
    }

    // NEW — start a new game
    if (strncmp(cmd, "NEW", 3) == 0) {
        uint8_t diff = 0;
        uint32_t opponentNode = 0;
        const char *arg = text + 3;
        while (*arg == ' ') arg++;

        if (*arg == '2') diff = 1;
        else if (*arg == '3') diff = 2;
        else if (*arg != '\0' && *arg != '1') {
            // Try to find player by name
            for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                if (!n || n->num == mp.from) continue;
                if (n->has_user) {
                    char sname[5] = {0};
                    strncpy(sname, n->user.short_name, 4);
                    for (int j = 0; sname[j]; j++) sname[j] = toupper((unsigned char)sname[j]);
                    char argUpper[5] = {0};
                    strncpy(argUpper, arg, 4);
                    for (int j = 0; argUpper[j]; j++) argUpper[j] = toupper((unsigned char)argUpper[j]);
                    if (strcmp(sname, argUpper) == 0) { opponentNode = n->num; break; }
                }
            }
            if (!opponentNode) {
                sendReply(mp, "Player not found.\nTry: NEW, NEW2, NEW3\nor NEW <shortname>");
                return ProcessMessage::STOP;
            }
        }

        BBSChessGame g;
        memset(&g, 0, sizeof(g));
        g.id         = chessNextGameId();
        g.difficulty = diff;
        g.status     = 0;
        g.toMove     = 0; // white moves first
        g.castling   = 0x0F;
        g.enPassantFile = -1;
        g.halfMoveClock = 0;
        g.fullMoveNumber = 1;
        g.lastMove   = (uint32_t)getTime();
        g.whiteNode  = mp.from;
        g.blackNode  = opponentNode; // 0 = AI

        chessBoardInit(g.board);

        if (!chessSaveGame(g)) {
            sendReply(mp, "Failed to create game.");
            return ProcessMessage::STOP;
        }

        session.chessGameId = g.id;

        char reply[120];
        if (opponentNode) {
            const char *oname = getNodeShortName(opponentNode);
            snprintf(reply, sizeof(reply), "Game #%u started!\nYou: White vs %s: Black\nYour turn first.",
                     g.id, oname ? oname : "opponent");
        } else {
            snprintf(reply, sizeof(reply), "Game #%u started!\nYou: White vs %s\nYour turn first.",
                     g.id, chessStatusName[diff]);
        }
        sendReply(mp, reply);
        delay(MULTIPART_DELAY_MS);
        char fen[100];
        chessBuildFEN(g, fen, sizeof(fen));
        sendReply(mp, fen);
        return ProcessMessage::STOP;
    }

    // Try to apply a move (e.g. e2e4, e7e8q)
    size_t tlen = strlen(text);
    if (tlen >= 4 && tlen <= 5) {
        if (session.chessGameId == 0) {
            sendReply(mp, "No game selected.\nType LG to list or NEW to start.");
            return ProcessMessage::STOP;
        }

        BBSChessGame g;
        if (!chessLoadGame(session.chessGameId, g)) {
            session.chessGameId = 0;
            sendReply(mp, "Game not found. Type LG.");
            return ProcessMessage::STOP;
        }

        if (g.status != 0) {
            sendReply(mp, "Game is over. Type LG or NEW.");
            return ProcessMessage::STOP;
        }

        bool iAmWhite = (g.whiteNode == mp.from);
        bool myTurn   = (iAmWhite && g.toMove == 0) || (!iAmWhite && g.toMove == 1);
        if (!myTurn) {
            // If it's an AI game stuck on the AI's turn, recover by running the AI now
            bool isAIGame = (g.blackNode == 0);
            if (isAIGame && g.toMove == 1) {
                ChessBoard boardBeforeAI;
                memcpy(boardBeforeAI, g.board, sizeof(ChessBoard));
                char aiMoveStr[6] = {0};
                bool aiMoved = chessAIMove(g, aiMoveStr);
                g.status = chessCheckTermination(g);
                chessSaveGame(g);
                char reply[200];
                if (aiMoved) {
                    char aiDesc[80];
                    chessMoveDescribe(boardBeforeAI, aiMoveStr, false, aiDesc, sizeof(aiDesc));
                    bool nextCheck = (g.status == 0) && chessIsInCheck(g.board, g.toMove == 0);
                    snprintf(reply, sizeof(reply), "AI move:\n%s\n%s", aiDesc,
                             g.status != 0 ? (g.status <= 2 ? (g.status == 1 ? "Checkmate! White wins!" : "Checkmate! Black wins!") : "Game over.")
                                           : (nextCheck ? "White to move. Check!" : "White to move."));
                } else {
                    snprintf(reply, sizeof(reply), "AI has no moves.");
                }
                sendReply(mp, reply);
                delay(MULTIPART_DELAY_MS);
                char fen[100]; chessBuildFEN(g, fen, sizeof(fen));
                sendReply(mp, fen);
                if (g.status != 0) chessUpdateRatings(g.whiteNode, g.blackNode, g.status, g.difficulty);
            } else {
                sendReply(mp, "Not your turn.");
            }
            return ProcessMessage::STOP;
        }

        char moveLower[6] = {0};
        strncpy(moveLower, text, 5);
        for (int i = 0; moveLower[i]; i++) moveLower[i] = tolower((unsigned char)moveLower[i]);

        // Save board state before player move for description
        ChessBoard boardBeforePlayer;
        memcpy(boardBeforePlayer, g.board, sizeof(ChessBoard));

        if (!chessApplyMove(g, moveLower)) {
            sendReply(mp, "Illegal move. Try again.\n(format: e2e4 or e7e8q)");
            return ProcessMessage::STOP;
        }

        char playerDesc[80];
        chessMoveDescribe(boardBeforePlayer, moveLower, true, playerDesc, sizeof(playerDesc));

        // Check game termination
        g.status = chessCheckTermination(g);
        chessSaveGame(g);

        bool aiGame = (g.blackNode == 0);

        if (g.status != 0) {
            static const char *endMsg[] = {"","Checkmate! White wins!","Checkmate! Black wins!","Draw!","Stalemate!"};
            char reply[120];
            snprintf(reply, sizeof(reply), "%s\n%s", playerDesc, endMsg[g.status]);
            sendReply(mp, reply);
            delay(MULTIPART_DELAY_MS);
            char fen[100]; chessBuildFEN(g, fen, sizeof(fen));
            sendReply(mp, fen);
            chessUpdateRatings(g.whiteNode, g.blackNode, g.status, g.difficulty);
            return ProcessMessage::STOP;
        }

        // If AI game and it's now AI's turn
        if (aiGame && g.toMove == 1) {
            ChessBoard boardBeforeAI;
            memcpy(boardBeforeAI, g.board, sizeof(ChessBoard));

            char aiMoveStr[6] = {0};
            bool aiMoved = chessAIMove(g, aiMoveStr);
            g.status = chessCheckTermination(g);
            chessSaveGame(g);

            char reply[200];
            if (aiMoved) {
                char aiDesc[80];
                chessMoveDescribe(boardBeforeAI, aiMoveStr, false, aiDesc, sizeof(aiDesc));
                bool nextCheck = (g.status == 0) && chessIsInCheck(g.board, g.toMove == 0);
                snprintf(reply, sizeof(reply), "%s\n%s\n%s",
                         playerDesc, aiDesc,
                         g.status != 0 ? (g.status <= 2 ? (g.status == 1 ? "Checkmate! White wins!" : "Checkmate! Black wins!") : "Game over.")
                                       : (nextCheck ? "White to move. Check!" : "White to move."));
            } else {
                snprintf(reply, sizeof(reply), "%s\nAI has no moves.", playerDesc);
            }
            sendReply(mp, reply);
            delay(MULTIPART_DELAY_MS);
            char fen[100]; chessBuildFEN(g, fen, sizeof(fen));
            sendReply(mp, fen);

            if (g.status != 0) chessUpdateRatings(g.whiteNode, g.blackNode, g.status, g.difficulty);
        } else {
            // PvP or waiting for opponent
            bool nextCheck = chessIsInCheck(g.board, g.toMove == 0);
            char reply[120];
            snprintf(reply, sizeof(reply), "%s\n%s to move.%s",
                     playerDesc, chessSideName[g.toMove], nextCheck ? " Check!" : "");
            sendReply(mp, reply);
            delay(MULTIPART_DELAY_MS);
            char fen[100]; chessBuildFEN(g, fen, sizeof(fen));
            sendReply(mp, fen);

            // Notify opponent if PvP
            if (g.whiteNode != 0 && g.blackNode != 0) {
                uint32_t oppNode = iAmWhite ? g.blackNode : g.whiteNode;
                char notif[120];
                const char *myName = getNodeShortName(mp.from);
                snprintf(notif, sizeof(notif), "Chess #%u: %s played %s. Your turn!",
                         g.id, myName ? myName : "opponent", moveLower);
                meshtastic_MeshPacket *pkt = allocDataPacket();
                if (pkt) {
                    pkt->to = oppNode;
                    pkt->want_ack = false;
                    size_t nlen = strlen(notif);
                    memcpy(pkt->decoded.payload.bytes, notif, nlen);
                    pkt->decoded.payload.size = nlen;
                    service->sendToMesh(pkt);
                }
            }
        }
        return ProcessMessage::STOP;
    }

    // FEN — show current FEN again
    if (strcmp(cmd, "FEN") == 0 && session.chessGameId != 0) {
        BBSChessGame g;
        if (chessLoadGame(session.chessGameId, g)) {
            char fen[100];
            chessBuildFEN(g, fen, sizeof(fen));
            sendReply(mp, fen);
        }
        return ProcessMessage::STOP;
    }

    sendChessStatus(mp, session);
    return ProcessMessage::STOP;
}


#if 0 // CASINO CODE REMOVED
static const char *bjCardStr(uint8_t v) {
    static const char *t[] = {"?","A","2","3","4","5","6","7","8","9","10","J","Q","K"};
    return (v >= 1 && v <= 13) ? t[v] : "?";
}

static uint8_t bjHandVal(const uint8_t *vals, uint8_t count) {
    int total = 0, aces = 0;
    for (int i = 0; i < count; i++) {
        if (vals[i] == 1) { aces++; total += 11; }
        else total += (vals[i] >= 10) ? 10 : (int)vals[i];
    }
    while (total > 21 && aces > 0) { total -= 10; aces--; }
    return (uint8_t)total;
}

// Writes hand like "A+K=21" or with hidden second card "A+?"
static int bjWriteHand(char *buf, int len, const uint8_t *vals, uint8_t count, bool hideSecond) {
    int p = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0 && p < len - 2) buf[p++] = '+';
        if (hideSecond && i == 1)
            p += snprintf(buf + p, len - p, "?");
        else
            p += snprintf(buf + p, len - p, "%s", bjCardStr(vals[i]));
    }
    if (!hideSecond)
        p += snprintf(buf + p, len - p, "=%d", bjHandVal(vals, count));
    return p;
}

static bool rouletteIsRed(int n) {
    static const uint8_t reds[] = {1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36};
    for (int i = 0; i < 18; i++) if (reds[i] == (uint8_t)n) return true;
    return false;
}

// ─── sendCasinoMenu ───────────────────────────────────────────────────────

void BBSModule::sendCasinoMenu(const meshtastic_MeshPacket &req, const BBSSession &session) {
    char buf[200];
    const char *label = session.casinoMode ? "New Vegas Casino" : "Casino";
    const char *chipLabel = session.casinoMode ? "caps" : "chips";
    snprintf(buf, sizeof(buf),
             "=== %s ===\n"
             "%u %s\n"
             "[B]lackjack [S]lots\n"
             "[R]oulette [X]Back",
             label, session.casinoChips, chipLabel);
    sendReply(req, buf);
}

// ─── doSlots ──────────────────────────────────────────────────────────────

void BBSModule::doSlots(const meshtastic_MeshPacket &req, BBSSession &session) {
    if (session.casinoChips < 5) {
        sendReply(req, "Need 5 chips to play slots!\n[X]Back");
        return;
    }
    session.casinoChips -= 5;

    // 5 slot symbols: 7, $, BAR, Cherry, Bell
    static const char *sym[] = {"7", "$", "BAR", "Chr", "Bel"};
    uint8_t s0 = (uint8_t)(random(0, 5));
    uint8_t s1 = (uint8_t)(random(0, 5));
    uint8_t s2 = (uint8_t)(random(0, 5));

    int win = 0;
    const char *msg = "";
    if (s0 == s1 && s1 == s2) {
        // Three of a kind
        switch (s0) {
            case 0: win = 50; msg = "*** JACKPOT! ***"; break;
            case 1: win = 30; msg = "BIG WIN!";         break;
            case 2: win = 20; msg = "Nice!";            break;
            case 3: win = 15; msg = "Sweet!";           break;
            default: win = 10; msg = "Winner!";         break;
        }
    } else if (s0 == s1 || s1 == s2 || s0 == s2) {
        // Two of a kind
        uint8_t pair = (s0 == s1) ? s0 : (s1 == s2 ? s1 : s0);
        switch (pair) {
            case 0: win = 14; msg = "Two 7s!";  break;
            case 1: win = 10; msg = "Two $!";   break;
            default: win = 5; msg = "Two match"; break;
        }
    }

    session.casinoChips += (uint16_t)win;
    const char *chipLabel = session.casinoMode ? "caps" : "chips";
    char buf[200];
    if (win > 0) {
        snprintf(buf, sizeof(buf),
                 "[%s][%s][%s]\n%s +%d\n%u %s\n[S]pin again [X]Back",
                 sym[s0], sym[s1], sym[s2], msg, win, session.casinoChips, chipLabel);
    } else {
        snprintf(buf, sizeof(buf),
                 "[%s][%s][%s]\nNo match. -5\n%u %s\n[S]pin again [X]Back",
                 sym[s0], sym[s1], sym[s2], session.casinoChips, chipLabel);
    }
    sendReply(req, buf);
}

// ─── handleStateCasino ────────────────────────────────────────────────────

ProcessMessage BBSModule::handleStateCasino(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendCasinoMenu(mp, session); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);
    char buf[200];

    switch (cmd) {
        case 'b': {
            // Start blackjack — bet 10 chips
            if (session.casinoChips < 10) {
                snprintf(buf, sizeof(buf), "Need 10 chips for blackjack!\nYou have %u.\n[X]Back", session.casinoChips);
                sendReply(mp, buf);
                break;
            }
            // Deal initial 4 cards
            session.bjPlayerCount = 0;
            session.bjDealerCount = 0;
            session.bjDoubled = 0;
            session.bjPlayerVals[session.bjPlayerCount++] = (uint8_t)random(1, 14);
            session.bjDealerVals[session.bjDealerCount++] = (uint8_t)random(1, 14);
            session.bjPlayerVals[session.bjPlayerCount++] = (uint8_t)random(1, 14);
            session.bjDealerVals[session.bjDealerCount++] = (uint8_t)random(1, 14);

            uint8_t pv = bjHandVal(session.bjPlayerVals, session.bjPlayerCount);
            char phand[30];
            bjWriteHand(phand, sizeof(phand), session.bjPlayerVals, session.bjPlayerCount, false);

            if (pv == 21) {
                // Natural blackjack! 3:2 on 10 bet = +15
                session.casinoChips += 15;
                char dhand[20];
                bjWriteHand(dhand, sizeof(dhand), session.bjDealerVals, session.bjDealerCount, false);
                snprintf(buf, sizeof(buf),
                         "BLACKJACK! +15\nYou:%s Dlr:%s\n%u chips\n[B]again [X]Back",
                         phand, dhand, session.casinoChips);
                sendReply(mp, buf);
            } else {
                session.state = BBS_STATE_BLACKJACK;
                snprintf(buf, sizeof(buf),
                         "-= Blackjack (bet:10) =-\nYou:%s\nDlr:%s+?\n[H]it [S]tand [D]ouble\n[X]Forfeit",
                         phand, bjCardStr(session.bjDealerVals[0]));
                sendReply(mp, buf);
            }
            break;
        }
        case 's':
            // Slots
            doSlots(mp, session);
            break;
        case 'r': {
            // Roulette — pick bet type
            session.state = BBS_STATE_ROULETTE;
            session.rlBetType = 0;
            if (session.casinoChips < 10) {
                snprintf(buf, sizeof(buf), "Need 10 chips for roulette!\nYou have %u.\n[X]Back", session.casinoChips);
                sendReply(mp, buf);
                session.state = BBS_STATE_CASINO;
                break;
            }
            snprintf(buf, sizeof(buf),
                     "-= Roulette (bet:10) =-\n%u chips\n[R]ed [B]lack\n[O]dd [E]ven\n[H]igh [L]ow\n1-36 for number (+100)\n[X]Back",
                     session.casinoChips);
            sendReply(mp, buf);
            break;
        }
        case 'x': {
            // Exit casino
            if (session.casinoMode == 1) {
                // Save chips back as caps
                FRPGPlayer plr;
                if (frpgLoadPlayer(mp.from, plr)) {
                    plr.caps = (session.casinoChips > 9999) ? 9999 : session.casinoChips;
                    frpgSavePlayer(plr);
                }
                session.state = BBS_STATE_WASTELAND;
                snprintf(buf, sizeof(buf), "Leaving New Vegas...\n%u caps saved.\n\nNV-Casino  EX-Explore\nSH-Shop  ST-Stats\n[X]Back to BBS",
                         session.casinoChips);
            } else {
                session.state = BBS_STATE_MAIN;
                snprintf(buf, sizeof(buf), "Cashed out with %u chips!\nHouse thanks you.", session.casinoChips);
            }
            sendReply(mp, buf);
            if (session.state == BBS_STATE_MAIN) {
                delay(MULTIPART_DELAY_MS);
                sendMainMenu(mp, session);
            }
            break;
        }
        default:
            sendCasinoMenu(mp, session);
    }
    return ProcessMessage::STOP;
}

// ─── handleStateBlackjack ─────────────────────────────────────────────────

ProcessMessage BBSModule::handleStateBlackjack(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    char buf[200];
    char phand[30], dhand[30];
    uint8_t bet = session.bjDoubled ? 20 : 10;
    const char *chipLabel = session.casinoMode ? "caps" : "chips";

    auto bjResult = [&](bool win, uint16_t amount, const char *msg) {
        if (win) {
            session.casinoChips += amount;
        } else {
            session.casinoChips = (session.casinoChips >= amount) ? session.casinoChips - amount : 0;
        }
        session.state = BBS_STATE_CASINO;
        bjWriteHand(phand, sizeof(phand), session.bjPlayerVals, session.bjPlayerCount, false);
        bjWriteHand(dhand, sizeof(dhand), session.bjDealerVals, session.bjDealerCount, false);
        snprintf(buf, sizeof(buf), "%s\nYou:%s Dlr:%s\n%u %s\n[B]again [X]Back",
                 msg, phand, dhand, session.casinoChips, chipLabel);
        sendReply(mp, buf);
    };

    if (!text || text[0] == '\0') {
        // Redisplay current hand
        bjWriteHand(phand, sizeof(phand), session.bjPlayerVals, session.bjPlayerCount, false);
        snprintf(buf, sizeof(buf), "You:%s\nDlr:%s+?\n[H]it [S]tand [D]ouble\n[X]Forfeit",
                 phand, bjCardStr(session.bjDealerVals[0]));
        sendReply(mp, buf);
        return ProcessMessage::STOP;
    }

    char cmd = tolower((unsigned char)text[0]);

    switch (cmd) {
        case 'h': {
            // Hit
            if (session.bjPlayerCount >= 6) {
                sendReply(mp, "Can't take more cards!");
                break;
            }
            session.bjPlayerVals[session.bjPlayerCount++] = (uint8_t)random(1, 14);
            uint8_t pv = bjHandVal(session.bjPlayerVals, session.bjPlayerCount);
            if (pv > 21) {
                bjResult(false, bet, "Bust! You lose.");
            } else if (pv == 21) {
                // Auto-stand at 21
                while (bjHandVal(session.bjDealerVals, session.bjDealerCount) < 17 && session.bjDealerCount < 6)
                    session.bjDealerVals[session.bjDealerCount++] = (uint8_t)random(1, 14);
                uint8_t dv = bjHandVal(session.bjDealerVals, session.bjDealerCount);
                if (dv > 21 || pv > dv) bjResult(true, bet, "You WIN!");
                else if (pv == dv) bjResult(false, 0, "Push! Tie.");
                else bjResult(false, bet, "Dealer wins.");
            } else {
                bjWriteHand(phand, sizeof(phand), session.bjPlayerVals, session.bjPlayerCount, false);
                snprintf(buf, sizeof(buf), "You:%s\nDlr:%s+?\n[H]it [S]tand [X]Forfeit",
                         phand, bjCardStr(session.bjDealerVals[0]));
                sendReply(mp, buf);
            }
            break;
        }
        case 's': {
            // Stand — dealer plays to 17+
            while (bjHandVal(session.bjDealerVals, session.bjDealerCount) < 17 && session.bjDealerCount < 6)
                session.bjDealerVals[session.bjDealerCount++] = (uint8_t)random(1, 14);
            uint8_t pv = bjHandVal(session.bjPlayerVals, session.bjPlayerCount);
            uint8_t dv = bjHandVal(session.bjDealerVals, session.bjDealerCount);
            if (dv > 21 || pv > dv) bjResult(true, bet, "You WIN!");
            else if (pv == dv) bjResult(false, 0, "Push! Tie.");
            else bjResult(false, bet, "Dealer wins.");
            break;
        }
        case 'd': {
            // Double down
            if (session.bjPlayerCount != 2 || session.bjDoubled) {
                sendReply(mp, "Can't double now.\n[H]it [S]tand [X]Forfeit");
                break;
            }
            if (session.casinoChips < 20) {
                snprintf(buf, sizeof(buf), "Need 20 chips to double.\nYou have %u.\n[H]it [S]tand", session.casinoChips);
                sendReply(mp, buf);
                break;
            }
            session.bjDoubled = 1;
            session.bjPlayerVals[session.bjPlayerCount++] = (uint8_t)random(1, 14);
            while (bjHandVal(session.bjDealerVals, session.bjDealerCount) < 17 && session.bjDealerCount < 6)
                session.bjDealerVals[session.bjDealerCount++] = (uint8_t)random(1, 14);
            uint8_t pv = bjHandVal(session.bjPlayerVals, session.bjPlayerCount);
            uint8_t dv = bjHandVal(session.bjDealerVals, session.bjDealerCount);
            if (pv > 21) bjResult(false, 20, "Bust! x2 loss.");
            else if (dv > 21 || pv > dv) bjResult(true, 20, "WIN! x2 payout!");
            else if (pv == dv) bjResult(false, 0, "Push! Tie.");
            else bjResult(false, 20, "Dealer wins x2.");
            break;
        }
        case 'x': {
            // Forfeit — lose the bet
            session.casinoChips = (session.casinoChips >= 10) ? session.casinoChips - 10 : 0;
            session.state = BBS_STATE_CASINO;
            snprintf(buf, sizeof(buf), "Forfeited. -10\n%u %s\n[B]again [X]Back", session.casinoChips, chipLabel);
            sendReply(mp, buf);
            break;
        }
        default: {
            bjWriteHand(phand, sizeof(phand), session.bjPlayerVals, session.bjPlayerCount, false);
            snprintf(buf, sizeof(buf), "You:%s\nDlr:%s+?\n[H]it [S]tand [D]ouble\n[X]Forfeit",
                     phand, bjCardStr(session.bjDealerVals[0]));
            sendReply(mp, buf);
        }
    }
    return ProcessMessage::STOP;
}

// ─── handleStateRoulette ──────────────────────────────────────────────────

ProcessMessage BBSModule::handleStateRoulette(const meshtastic_MeshPacket &mp, BBSSession &session, const char *text) {
    char buf[200];
    const char *chipLabel = session.casinoMode ? "caps" : "chips";

    if (!text || text[0] == '\0') {
        snprintf(buf, sizeof(buf),
                 "-= Roulette =-\n%u %s\n[R]ed [B]lack [O]dd [E]ven\n[H]igh [L]ow\n1-36 number(+100)\n[X]Back",
                 session.casinoChips, chipLabel);
        sendReply(mp, buf);
        return ProcessMessage::STOP;
    }

    // Check if it's a number bet (1-36)
    int numBet = atoi(text);
    if (numBet >= 1 && numBet <= 36) {
        session.rlBetType = 7;
        session.rlBetNum = (uint8_t)numBet;
    } else {
        char cmd = tolower((unsigned char)text[0]);
        switch (cmd) {
            case 'r': session.rlBetType = 1; break;
            case 'b': session.rlBetType = 2; break;
            case 'o': session.rlBetType = 3; break;
            case 'e': session.rlBetType = 4; break;
            case 'h': session.rlBetType = 5; break;
            case 'l': session.rlBetType = 6; break;
            case 'x':
                session.state = BBS_STATE_CASINO;
                sendCasinoMenu(mp, session);
                return ProcessMessage::STOP;
            default:
                snprintf(buf, sizeof(buf),
                         "-= Roulette =-\n[R]ed [B]lack [O]dd [E]ven\n[H]igh [L]ow\n1-36 number\n[X]Back");
                sendReply(mp, buf);
                return ProcessMessage::STOP;
        }
    }

    // Spin the wheel (0-36)
    int ball = (int)random(0, 37);
    bool isRed = (ball > 0) && rouletteIsRed(ball);
    bool isBlack = (ball > 0) && !isRed;
    bool isOdd = (ball > 0) && (ball % 2 == 1);
    bool isHigh = (ball >= 19);

    const char *color = (ball == 0) ? "Green" : (isRed ? "Red" : "Black");
    bool playerWins = false;
    bool isNumberBet = (session.rlBetType == 7);

    switch (session.rlBetType) {
        case 1: playerWins = isRed;   break;
        case 2: playerWins = isBlack; break;
        case 3: playerWins = isOdd;   break;
        case 4: playerWins = (ball > 0) && !isOdd; break;
        case 5: playerWins = isHigh;  break;
        case 6: playerWins = (ball > 0) && !isHigh; break;
        case 7: playerWins = (ball == session.rlBetNum); break;
    }

    static const char *betNames[] = {"","Red","Black","Odd","Even","High","Low","Number"};
    const char *betName = betNames[session.rlBetType];

    if (playerWins) {
        uint16_t payout = isNumberBet ? 350 : 10;  // 35:1 for number, 1:1 for even bets
        session.casinoChips += payout;
        snprintf(buf, sizeof(buf),
                 "Ball: %d %s!\nBet:%s WIN +%u\n%u %s\n[R]again [X]Back",
                 ball, color, betName, payout, session.casinoChips, chipLabel);
    } else {
        session.casinoChips = (session.casinoChips >= 10) ? session.casinoChips - 10 : 0;
        snprintf(buf, sizeof(buf),
                 "Ball: %d %s\nBet:%s - LOSE -10\n%u %s\n[R]again [X]Back",
                 ball, color, betName, session.casinoChips, chipLabel);
    }

    session.state = BBS_STATE_CASINO;
    sendReply(mp, buf);

    // If player presses [R]again, it'll hit the casino handler and go back to roulette
    return ProcessMessage::STOP;
}
#endif // CASINO CODE REMOVED

#if HAS_SCREEN
void BBSModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Title bar
    if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_INVERTED)
        display->fillRect(x, y, display->getWidth(), FONT_HEIGHT_SMALL);
    display->setColor(OLEDDISPLAY_COLOR::BLACK);
#ifdef NRF52_SERIES
    display->drawString(x, y, "BBSN");
#else
    display->drawString(x, y, "TinyBBS");
#endif
    display->setColor(OLEDDISPLAY_COLOR::WHITE);

    if (!storage_) {
        display->drawString(x, y + FONT_HEIGHT_SMALL, "Storage offline");
        return;
    }

    // All heavy work is done in runOnce() — just read cached values here
    uint32_t now = (uint32_t)getTime();

    // Active sessions (cheap loop, no I/O)
    uint32_t activeSessions = 0;
    for (int i = 0; i < BBS_MAX_SESSIONS; i++) {
        if (sessions_[i].nodeNum != 0 &&
            sessions_[i].state != BBS_STATE_IDLE &&
            (now - sessions_[i].lastActivity) < BBS_SESSION_TIMEOUT_S)
            activeSessions++;
    }

    char line1[32], line2[32], line3[32];
    snprintf(line1, sizeof(line1), "Bulletins:%u(%u) Mail:%u", (unsigned)uiBulletinTotal_, (unsigned)uiBulletinRecent_, (unsigned)uiMailTotal_);
    snprintf(line2, sizeof(line2), "Sessions: %u", (unsigned)activeSessions);

    if (uiLastMsgTime_ && uiLastMsgFrom_[0]) {
        uint32_t ago = (now > uiLastMsgTime_) ? (now - uiLastMsgTime_) : 0;
        char agoStr[10];
        if (ago < 60)        snprintf(agoStr, sizeof(agoStr), "%us", (unsigned)ago);
        else if (ago < 3600) snprintf(agoStr, sizeof(agoStr), "%um", (unsigned)(ago / 60));
        else                 snprintf(agoStr, sizeof(agoStr), "%uh", (unsigned)(ago / 3600));
        snprintf(line3, sizeof(line3), "Last: %s %s ago", uiLastMsgFrom_, agoStr);
    } else {
        snprintf(line3, sizeof(line3), "Last: none");
    }

    display->drawString(x, y + FONT_HEIGHT_SMALL,     line1);
    display->drawString(x, y + FONT_HEIGHT_SMALL * 2, line2);
    display->drawString(x, y + FONT_HEIGHT_SMALL * 3, line3);
}
#endif // HAS_SCREEN
