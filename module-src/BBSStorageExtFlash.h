#pragma once
// LittleFS storage backend on external QSPI flash (nRF52840 / T-Echo)
//
// Identical API to BBSStorageLittleFS but uses bbsExtFS (2MB external flash)
// instead of FSCom (tiny internal flash).  This keeps BBS data completely
// separate from Meshtastic's own filesystem.
//
// File layout mirrors BBSStorageLittleFS:
//   /bbs/meta.bin, /bbs/bul/XXXX.bin, /bbs/mail/<hex>/XXXX.bin, /bbs/qsl/XXXX.bin

#ifdef NRF52_SERIES

#include "BBSStorage.h"
#include "BBSExtFlash.h"
#include "DebugConfiguration.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <climits>

// Capacity: external flash is much larger — use the same limits as ESP32 flash
#define EXTFLASH_MAX_BULLETINS     50
#define EXTFLASH_MAX_MAIL_PER_USER 20
#define EXTFLASH_MAX_QSL           50

#define EXT_BBS_BASE_PATH      "/bbs"
#define EXT_BBS_BULLETIN_PATH  "/bbs/bul"
#define EXT_BBS_MAIL_PATH      "/bbs/mail"
#define EXT_BBS_QSL_PATH       "/bbs/qsl"
#define EXT_BBS_WORDLE_PATH    "/bbs/wdl"
#define EXT_BBS_META_PATH      "/bbs/meta.bin"
#define EXT_BBS_META_MAGIC     0xBB50EF01  // different magic to avoid confusion with internal

class BBSStorageExtFlash : public BBSStorage {
  private:
    uint32_t nextBulletinId_ = 1;
    uint32_t nextMailId_ = 1;
    uint32_t nextQSLId_ = 1;
    bool initialized_ = false;

    // All filesystem ops go through bbsExtFS (external flash), not FSCom
    bool ensureDir(const char *path) {
        if (!bbsExtFS().exists(path)) return bbsExtFS().mkdir(path);
        return true;
    }

    bool loadMetadata() {
        File f = bbsExtFS().open(EXT_BBS_META_PATH, FILE_O_READ);
        if (!f) return false;
        uint32_t magic = 0;
        f.read((uint8_t *)&magic, sizeof(uint32_t));
        if (magic != EXT_BBS_META_MAGIC) { f.close(); return false; }
        f.read((uint8_t *)&nextBulletinId_, sizeof(uint32_t));
        f.read((uint8_t *)&nextMailId_, sizeof(uint32_t));
        if (f.read((uint8_t *)&nextQSLId_, sizeof(uint32_t)) != sizeof(uint32_t)) nextQSLId_ = 1;
        f.close();
        return true;
    }

    bool saveMetadata() {
        File f = bbsExtFS().open(EXT_BBS_META_PATH, FILE_O_WRITE);
        if (!f) return false;
        uint32_t magic = EXT_BBS_META_MAGIC;
        f.write((const uint8_t *)&magic, sizeof(uint32_t));
        f.write((const uint8_t *)&nextBulletinId_, sizeof(uint32_t));
        f.write((const uint8_t *)&nextMailId_, sizeof(uint32_t));
        f.write((const uint8_t *)&nextQSLId_, sizeof(uint32_t));
        f.close();
        return true;
    }

    void getBulletinPath(uint32_t id, char *path, size_t len) {
        snprintf(path, len, "%s/%04" PRIu32 ".bin", EXT_BBS_BULLETIN_PATH, id);
    }
    void getMailDirPath(uint32_t nodeNum, char *path, size_t len) {
        snprintf(path, len, "%s/%08" PRIx32, EXT_BBS_MAIL_PATH, nodeNum);
    }
    void getMailPath(uint32_t nodeNum, uint32_t mailId, char *path, size_t len) {
        char dir[64]; getMailDirPath(nodeNum, dir, sizeof(dir));
        snprintf(path, len, "%s/%04" PRIu32 ".bin", dir, mailId);
    }
    void getQSLPath(uint32_t id, char *path, size_t len) {
        snprintf(path, len, "%s/%04" PRIu32 ".bin", EXT_BBS_QSL_PATH, id);
    }

    uint32_t countFiles(const char *dirPath) {
        File dir = bbsExtFS().open(dirPath, FILE_O_READ);
        if (!dir) return 0;
        uint32_t count = 0;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) count++;
            f.close();
        }
        dir.close();
        return count;
    }

    void deleteOldestFile(const char *dirPath) {
        uint32_t oldestId = UINT32_MAX;
        char oldestPath[80] = {0};
        File dir = bbsExtFS().open(dirPath, FILE_O_READ);
        if (!dir) return;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                uint32_t id = strtoul(f.name(), nullptr, 10);
                if (id < oldestId) {
                    oldestId = id;
                    snprintf(oldestPath, sizeof(oldestPath), "%s/%s", dirPath, f.name());
                }
            }
            f.close();
        }
        dir.close();
        if (oldestId != UINT32_MAX && oldestPath[0]) bbsExtFS().remove(oldestPath);
    }

  public:
    BBSStorageExtFlash() {}
    ~BBSStorageExtFlash() {}

    bool init() override {
        // Initialize the external flash filesystem
        if (!bbsExtFS().begin()) {
            LOG_ERROR("[BBS-ExtFlash] Failed to mount external flash\n");
            return false;
        }

        if (!ensureDir(EXT_BBS_BASE_PATH) || !ensureDir(EXT_BBS_BULLETIN_PATH) ||
            !ensureDir(EXT_BBS_MAIL_PATH) || !ensureDir(EXT_BBS_QSL_PATH) ||
            !ensureDir(EXT_BBS_WORDLE_PATH)) {
            LOG_ERROR("[BBS-ExtFlash] Failed to create directories\n");
            return false;
        }
        if (!loadMetadata()) {
            nextBulletinId_ = 1; nextMailId_ = 1; nextQSLId_ = 1;
            saveMetadata();
        }
        initialized_ = true;

        uint32_t used = bbsExtFS().usedBytes();
        uint32_t total = bbsExtFS().totalBytes();
        LOG_INFO("[BBS-ExtFlash] Ready: %lu/%lu bytes used (%lu%% free)\n",
                 (unsigned long)used, (unsigned long)total,
                 (unsigned long)((total - used) * 100 / total));
        return true;
    }

    // ── BULLETINS ──────────────────────────────────────────────────────────

    bool storeBulletin(const BBSBulletin &bulletin) override {
        if (!initialized_) return false;
        if (countFiles(EXT_BBS_BULLETIN_PATH) >= EXTFLASH_MAX_BULLETINS) {
            deleteOldestFile(EXT_BBS_BULLETIN_PATH);
        }
        char path[64]; getBulletinPath(bulletin.id, path, sizeof(path));
        File f = bbsExtFS().open(path, FILE_O_WRITE);
        if (!f) return false;
        f.write((const uint8_t *)&bulletin.id, sizeof(uint32_t));
        f.write((const uint8_t *)&bulletin.authorNode, sizeof(uint32_t));
        f.write((const uint8_t *)bulletin.authorName, BBS_SHORT_NAME_LEN);
        f.write((const uint8_t *)&bulletin.timestamp, sizeof(uint32_t));
        f.write((const uint8_t *)bulletin.body, BBS_MSG_MAX_LEN + 1);
        f.write((const uint8_t *)&bulletin.board, sizeof(uint8_t));
        f.close();
        saveMetadata();
        return true;
    }

    bool loadBulletin(uint32_t id, BBSBulletin &bulletin) override {
        if (!initialized_) return false;
        char path[64]; getBulletinPath(id, path, sizeof(path));
        File f = bbsExtFS().open(path, FILE_O_READ);
        if (!f) return false;
        f.read((uint8_t *)&bulletin.id, sizeof(uint32_t));
        f.read((uint8_t *)&bulletin.authorNode, sizeof(uint32_t));
        f.read((uint8_t *)bulletin.authorName, BBS_SHORT_NAME_LEN);
        f.read((uint8_t *)&bulletin.timestamp, sizeof(uint32_t));
        f.read((uint8_t *)bulletin.body, BBS_MSG_MAX_LEN + 1);
        bulletin.board = BOARD_GENERAL;
        f.read((uint8_t *)&bulletin.board, sizeof(uint8_t));
        bulletin.active = true;
        f.close();
        return true;
    }

    bool deleteBulletin(uint32_t id, uint32_t requestorNodeNum) override {
        if (!initialized_) return false;
        BBSBulletin b;
        if (!loadBulletin(id, b)) return false;
        if (b.authorNode != requestorNodeNum) return false;
        char path[64]; getBulletinPath(id, path, sizeof(path));
        return bbsExtFS().remove(path);
    }

    uint32_t listBulletins(BBSBulletinHeader *headers, uint32_t maxResults, uint32_t offset = 0, uint8_t board = BOARD_ALL) override {
        if (!headers || maxResults == 0 || !initialized_) return 0;
        File dir = bbsExtFS().open(EXT_BBS_BULLETIN_PATH, FILE_O_READ);
        if (!dir) return 0;

        uint32_t ids[EXTFLASH_MAX_BULLETINS];
        uint32_t idCount = 0;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory() && idCount < EXTFLASH_MAX_BULLETINS) {
                uint32_t id = 0; sscanf(f.name(), "%04" PRIu32, &id);
                ids[idCount++] = id;
            }
            f.close();
        }
        dir.close();

        // Sort descending
        for (uint32_t i = 0; i + 1 < idCount; i++)
            for (uint32_t j = i + 1; j < idCount; j++)
                if (ids[i] < ids[j]) { uint32_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }

        uint32_t result = 0, skipped = 0;
        for (uint32_t i = 0; i < idCount && result < maxResults; i++) {
            BBSBulletin b;
            if (!loadBulletin(ids[i], b)) continue;
            if (board != BOARD_ALL && b.board != board) continue;
            if (skipped < offset) { skipped++; continue; }
            headers[result].id = b.id;
            headers[result].authorNode = b.authorNode;
            strncpy(headers[result].authorName, b.authorName, BBS_SHORT_NAME_LEN - 1);
            headers[result].authorName[BBS_SHORT_NAME_LEN - 1] = '\0';
            headers[result].timestamp = b.timestamp;
            strncpy(headers[result].preview, b.body, BBS_PREVIEW_LEN);
            headers[result].preview[BBS_PREVIEW_LEN] = '\0';
            headers[result].board = b.board;
            result++;
        }
        return result;
    }

    uint32_t totalActiveBulletins(uint8_t board = BOARD_ALL) override {
        if (!initialized_) return 0;
        if (board == BOARD_ALL) return countFiles(EXT_BBS_BULLETIN_PATH);
        // Count per-board without allocating a big headers array (stack-safe)
        File dir = bbsExtFS().open(EXT_BBS_BULLETIN_PATH, FILE_O_READ);
        if (!dir) return 0;
        uint32_t count = 0;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                // Read just the board byte (last byte of bulletin record)
                // Layout: id(4) + authorNode(4) + authorName(12) + timestamp(4) + body(201) + board(1) = 226
                uint8_t boardByte = BOARD_GENERAL;
                size_t fsize = f.size();
                if (fsize >= 226) {
                    f.seek(225);
                    f.read(&boardByte, 1);
                }
                if (boardByte == board) count++;
            }
            f.close();
        }
        dir.close();
        return count;
    }

    // ── MAIL ───────────────────────────────────────────────────────────────

    bool storeMail(const BBSMailMsg &msg) override {
        if (!initialized_) return false;
        char dirPath[64]; getMailDirPath(msg.toNode, dirPath, sizeof(dirPath));
        if (countFiles(dirPath) >= EXTFLASH_MAX_MAIL_PER_USER) deleteOldestFile(dirPath);
        if (!ensureDir(dirPath)) return false;

        char path[80]; getMailPath(msg.toNode, msg.id, path, sizeof(path));
        File f = bbsExtFS().open(path, FILE_O_WRITE);
        if (!f) return false;
        f.write((const uint8_t *)&msg.id, sizeof(uint32_t));
        f.write((const uint8_t *)&msg.fromNode, sizeof(uint32_t));
        f.write((const uint8_t *)msg.fromName, BBS_SHORT_NAME_LEN);
        f.write((const uint8_t *)&msg.toNode, sizeof(uint32_t));
        f.write((const uint8_t *)&msg.timestamp, sizeof(uint32_t));
        f.write((const uint8_t *)msg.subject, BBS_SUBJECT_LEN);
        f.write((const uint8_t *)msg.body, BBS_MSG_MAX_LEN + 1);
        uint8_t read_flag = msg.read ? 1 : 0;
        f.write((const uint8_t *)&read_flag, sizeof(uint8_t));
        f.close();
        saveMetadata();
        return true;
    }

    bool loadMail(uint32_t id, uint32_t recipientNode, BBSMailMsg &msg) override {
        if (!initialized_) return false;
        char path[80]; getMailPath(recipientNode, id, path, sizeof(path));
        File f = bbsExtFS().open(path, FILE_O_READ);
        if (!f) return false;
        f.read((uint8_t *)&msg.id, sizeof(uint32_t));
        f.read((uint8_t *)&msg.fromNode, sizeof(uint32_t));
        f.read((uint8_t *)msg.fromName, BBS_SHORT_NAME_LEN);
        f.read((uint8_t *)&msg.toNode, sizeof(uint32_t));
        f.read((uint8_t *)&msg.timestamp, sizeof(uint32_t));
        memset(msg.subject, 0, BBS_SUBJECT_LEN);
        f.read((uint8_t *)msg.subject, BBS_SUBJECT_LEN);
        f.read((uint8_t *)msg.body, BBS_MSG_MAX_LEN + 1);
        uint8_t read_flag = 0;
        f.read((uint8_t *)&read_flag, sizeof(uint8_t));
        msg.read = (read_flag != 0);
        msg.active = true;
        f.close();
        return true;
    }

    bool deleteMail(uint32_t id, uint32_t recipientNode) override {
        if (!initialized_) return false;
        BBSMailMsg msg;
        if (!loadMail(id, recipientNode, msg)) return false;
        if (msg.toNode != recipientNode) return false;
        char path[80]; getMailPath(recipientNode, id, path, sizeof(path));
        return bbsExtFS().remove(path);
    }

    bool markMailRead(uint32_t id, uint32_t recipientNode) override {
        BBSMailMsg msg;
        if (!loadMail(id, recipientNode, msg)) return false;
        msg.read = true;
        return storeMail(msg);
    }

    uint32_t listMail(uint32_t recipientNode, BBSMailHeader *headers, uint32_t maxResults, uint32_t offset = 0) override {
        if (!headers || maxResults == 0 || !initialized_) return 0;
        char dirPath[64]; getMailDirPath(recipientNode, dirPath, sizeof(dirPath));
        File dir = bbsExtFS().open(dirPath, FILE_O_READ);
        if (!dir) return 0;

        uint32_t ids[EXTFLASH_MAX_MAIL_PER_USER];
        uint32_t idCount = 0;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory() && idCount < EXTFLASH_MAX_MAIL_PER_USER) {
                uint32_t id = 0; sscanf(f.name(), "%04" PRIu32, &id);
                ids[idCount++] = id;
            }
            f.close();
        }
        dir.close();

        for (uint32_t i = 0; i + 1 < idCount; i++)
            for (uint32_t j = i + 1; j < idCount; j++)
                if (ids[i] < ids[j]) { uint32_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }

        uint32_t result = 0;
        for (uint32_t i = offset; i < idCount && result < maxResults; i++, result++) {
            BBSMailMsg msg;
            if (loadMail(ids[i], recipientNode, msg)) {
                headers[result].id = msg.id;
                headers[result].fromNode = msg.fromNode;
                strncpy(headers[result].fromName, msg.fromName, BBS_SHORT_NAME_LEN - 1);
                headers[result].fromName[BBS_SHORT_NAME_LEN - 1] = '\0';
                headers[result].timestamp = msg.timestamp;
                strncpy(headers[result].subject, msg.subject, BBS_SUBJECT_LEN - 1);
                headers[result].subject[BBS_SUBJECT_LEN - 1] = '\0';
                headers[result].read = msg.read;
            }
        }
        return result;
    }

    uint32_t countUnreadMail(uint32_t recipientNode) override {
        if (!initialized_) return 0;
        char dirPath[64]; getMailDirPath(recipientNode, dirPath, sizeof(dirPath));
        File dir = bbsExtFS().open(dirPath, FILE_O_READ);
        if (!dir) return 0;
        uint32_t count = 0;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                uint32_t fileId = 0; sscanf(f.name(), "%04" PRIu32, &fileId);
                BBSMailMsg msg;
                if (loadMail(fileId, recipientNode, msg) && !msg.read) count++;
            }
            f.close();
        }
        dir.close();
        return count;
    }

    // ── QSL ────────────────────────────────────────────────────────────────

    bool storeQSL(const BBSQSL &qsl) override {
        if (!initialized_) return false;
        if (countFiles(EXT_BBS_QSL_PATH) >= EXTFLASH_MAX_QSL) deleteOldestFile(EXT_BBS_QSL_PATH);
        char path[64]; getQSLPath(qsl.id, path, sizeof(path));
        File f = bbsExtFS().open(path, FILE_O_WRITE);
        if (!f) return false;
        f.write((const uint8_t *)&qsl.id, sizeof(uint32_t));
        f.write((const uint8_t *)&qsl.fromNode, sizeof(uint32_t));
        f.write((const uint8_t *)qsl.fromName, BBS_SHORT_NAME_LEN);
        f.write((const uint8_t *)&qsl.latitude, sizeof(int32_t));
        f.write((const uint8_t *)&qsl.longitude, sizeof(int32_t));
        f.write((const uint8_t *)&qsl.altitude, sizeof(int32_t));
        f.write((const uint8_t *)&qsl.timestamp, sizeof(uint32_t));
        f.write((const uint8_t *)&qsl.hopsAway, sizeof(uint8_t));
        f.write((const uint8_t *)&qsl.snr, sizeof(uint8_t));
        f.write((const uint8_t *)&qsl.rssi, sizeof(int8_t));
        f.write((const uint8_t *)qsl.location, sizeof(qsl.location));
        f.close();
        saveMetadata();
        return true;
    }

    uint32_t listQSL(BBSQSLHeader *headers, uint32_t maxResults, uint32_t offset = 0) override {
        if (!headers || maxResults == 0 || !initialized_) return 0;
        File dir = bbsExtFS().open(EXT_BBS_QSL_PATH, FILE_O_READ);
        if (!dir) return 0;
        uint32_t ids[EXTFLASH_MAX_QSL];
        uint32_t idCount = 0;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory() && idCount < EXTFLASH_MAX_QSL) {
                uint32_t id = 0; sscanf(f.name(), "%04" PRIu32, &id); ids[idCount++] = id;
            }
            f.close();
        }
        dir.close();
        for (uint32_t i = 0; i + 1 < idCount; i++)
            for (uint32_t j = i + 1; j < idCount; j++)
                if (ids[i] < ids[j]) { uint32_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
        uint32_t result = 0;
        for (uint32_t i = offset; i < idCount && result < maxResults; i++, result++) {
            char path[64]; getQSLPath(ids[i], path, sizeof(path));
            File qf = bbsExtFS().open(path, FILE_O_READ);
            if (qf) {
                BBSQSL qsl;
                qf.read((uint8_t *)&qsl.id, sizeof(uint32_t));
                qf.read((uint8_t *)&qsl.fromNode, sizeof(uint32_t));
                qf.read((uint8_t *)qsl.fromName, BBS_SHORT_NAME_LEN);
                qf.read((uint8_t *)&qsl.latitude, sizeof(int32_t));
                qf.read((uint8_t *)&qsl.longitude, sizeof(int32_t));
                qf.read((uint8_t *)&qsl.altitude, sizeof(int32_t));
                qf.read((uint8_t *)&qsl.timestamp, sizeof(uint32_t));
                qf.read((uint8_t *)&qsl.hopsAway, sizeof(uint8_t));
                qf.read((uint8_t *)&qsl.snr, sizeof(uint8_t));
                qf.read((uint8_t *)&qsl.rssi, sizeof(int8_t));
                qf.read((uint8_t *)qsl.location, sizeof(qsl.location));
                qsl.location[sizeof(qsl.location) - 1] = '\0';
                qf.close();
                headers[result].id = qsl.id;
                headers[result].fromNode = qsl.fromNode;
                strncpy(headers[result].fromName, qsl.fromName, BBS_SHORT_NAME_LEN - 1);
                headers[result].fromName[BBS_SHORT_NAME_LEN - 1] = '\0';
                headers[result].timestamp = qsl.timestamp;
                headers[result].hopsAway = qsl.hopsAway;
                headers[result].hasLocation = (qsl.latitude != 0 || qsl.longitude != 0);
                strncpy(headers[result].location, qsl.location, sizeof(headers[result].location) - 1);
                headers[result].location[sizeof(headers[result].location) - 1] = '\0';
            }
        }
        return result;
    }

    uint32_t totalActiveQSL() override {
        if (!initialized_) return 0;
        return countFiles(EXT_BBS_QSL_PATH);
    }

    uint32_t pruneExpiredQSL(uint32_t currentTime) override {
        if (!initialized_) return 0;
        File dir = bbsExtFS().open(EXT_BBS_QSL_PATH, FILE_O_READ);
        if (!dir) return 0;
        uint32_t pruned = 0;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                uint32_t fileId = 0; sscanf(f.name(), "%04" PRIu32, &fileId);
                char path[64]; getQSLPath(fileId, path, sizeof(path));
                File qf = bbsExtFS().open(path, FILE_O_READ);
                if (qf) {
                    BBSQSL qsl; memset(&qsl, 0, sizeof(qsl));
                    qf.read((uint8_t *)&qsl.id, sizeof(uint32_t));
                    qf.read((uint8_t *)&qsl.fromNode, sizeof(uint32_t));
                    qf.read((uint8_t *)qsl.fromName, BBS_SHORT_NAME_LEN);
                    qf.read((uint8_t *)&qsl.latitude, sizeof(int32_t));
                    qf.read((uint8_t *)&qsl.longitude, sizeof(int32_t));
                    qf.read((uint8_t *)&qsl.altitude, sizeof(int32_t));
                    qf.read((uint8_t *)&qsl.timestamp, sizeof(uint32_t));
                    qf.close();
                    if ((currentTime - qsl.timestamp) > QSL_TTL_SECONDS) {
                        bbsExtFS().remove(path); pruned++;
                    }
                }
            }
            f.close();
        }
        dir.close();
        return pruned;
    }

    uint32_t nextQSLId() override { uint32_t id = nextQSLId_++; saveMetadata(); return id; }
    bool compact() override { return true; }

    BBSStats getStats() override {
        BBSStats stats;
        uint32_t total = bbsExtFS().totalBytes();
        uint32_t used  = bbsExtFS().usedBytes();
        stats.freeBytesEstimate = (total > used) ? (total - used) : 0;
        stats.totalBulletins = totalActiveBulletins();
        stats.totalMailItems = 0;
        stats.totalQSLItems = totalActiveQSL();
        stats.maxBulletins = EXTFLASH_MAX_BULLETINS;
        stats.maxMailPerUser = EXTFLASH_MAX_MAIL_PER_USER;
        return stats;
    }

    uint32_t nextBulletinId() override { uint32_t id = nextBulletinId_++; saveMetadata(); return id; }
    uint32_t nextMailId() override { uint32_t id = nextMailId_++; saveMetadata(); return id; }
};

#endif // NRF52_SERIES
