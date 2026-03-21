#pragma once

#include "BBSStorage.h"
#include "DebugConfiguration.h"
#include "FSCommon.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <climits>

#define BBS_BASE_PATH "/bbs"
#define BBS_BULLETIN_PATH "/bbs/bul"
#define BBS_MAIL_PATH "/bbs/mail"
#define BBS_QSL_PATH "/bbs/qsl"
#define BBS_WORDLE_PATH "/bbs/wdl"
#define BBS_META_PATH "/bbs/meta.bin"
#define BBS_META_MAGIC 0xBB500002  // bumped from 0xBB500001 to invalidate old format

/**
 * LittleFS-based persistent storage
 *   /bbs/meta.bin        - nextBulletinId, nextMailId, nextQSLId
 *   /bbs/bul/XXXX.bin   - bulletin with ID XXXX
 *   /bbs/mail/<hex>/XXXX.bin - mail to node
 *   /bbs/qsl/XXXX.bin   - QSL entry
 *
 * Bulletin binary format (226 bytes):
 *   id(4) authorNode(4) authorName(12) timestamp(4) body(201) board(1)
 *   board appended last for backward compat (defaults to BOARD_GENERAL on short read)
 *
 * Mail binary format (262 bytes):
 *   id(4) fromNode(4) fromName(12) toNode(4) timestamp(4) subject(32) body(201) read(1)
 */
class BBSStorageLittleFS : public BBSStorage {
  private:
    uint32_t nextBulletinId_ = 1;
    uint32_t nextMailId_ = 1;
    uint32_t nextQSLId_ = 1;
    bool initialized_ = false;

    bool ensureDir(const char *path) {
        if (!FSCom.exists(path)) return FSCom.mkdir(path);
        return true;
    }

    bool loadMetadata() {
        File f = FSCom.open(BBS_META_PATH, FILE_O_READ);
        if (!f) return false;
        uint32_t magic = 0;
        f.read((uint8_t *)&magic, sizeof(uint32_t));
        if (magic != BBS_META_MAGIC) { f.close(); return false; }
        f.read((uint8_t *)&nextBulletinId_, sizeof(uint32_t));
        f.read((uint8_t *)&nextMailId_, sizeof(uint32_t));
        if (f.read((uint8_t *)&nextQSLId_, sizeof(uint32_t)) != sizeof(uint32_t)) nextQSLId_ = 1;
        f.close();
        return true;
    }

    bool saveMetadata() {
        File f = FSCom.open(BBS_META_PATH, FILE_O_WRITE);
        if (!f) return false;
        uint32_t magic = BBS_META_MAGIC;
        f.write((const uint8_t *)&magic, sizeof(uint32_t));
        f.write((const uint8_t *)&nextBulletinId_, sizeof(uint32_t));
        f.write((const uint8_t *)&nextMailId_, sizeof(uint32_t));
        f.write((const uint8_t *)&nextQSLId_, sizeof(uint32_t));
        f.close();
        return true;
    }

    void getBulletinPath(uint32_t id, char *path, size_t len) {
        snprintf(path, len, "%s/%04" PRIu32 ".bin", BBS_BULLETIN_PATH, id);
    }
    void getMailDirPath(uint32_t nodeNum, char *path, size_t len) {
        snprintf(path, len, "%s/%08" PRIx32, BBS_MAIL_PATH, nodeNum);
    }
    void getMailPath(uint32_t nodeNum, uint32_t mailId, char *path, size_t len) {
        char dir[64]; getMailDirPath(nodeNum, dir, sizeof(dir));
        snprintf(path, len, "%s/%04" PRIu32 ".bin", dir, mailId);
    }
    void getQSLPath(uint32_t id, char *path, size_t len) {
        snprintf(path, len, "%s/%04" PRIu32 ".bin", BBS_QSL_PATH, id);
    }

    uint32_t countFiles(const char *dirPath) {
        File dir = FSCom.open(dirPath, FILE_O_READ);
        if (!dir) return 0;
        uint32_t count = 0;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) count++;
            f.close();
        }
        dir.close();
        return count;
    }

    void deleteOldestBulletin() {
        uint32_t oldestId = UINT32_MAX;
        char oldestPath[64] = {0};
        File dir = FSCom.open(BBS_BULLETIN_PATH, FILE_O_READ);
        if (!dir) return;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                uint32_t id = strtoul(f.name(), nullptr, 10);
                if (id < oldestId) {
                    oldestId = id;
                    snprintf(oldestPath, sizeof(oldestPath), "%s/%s", BBS_BULLETIN_PATH, f.name());
                }
            }
            f.close();
        }
        dir.close();
        if (oldestId != UINT32_MAX && oldestPath[0]) FSCom.remove(oldestPath);
    }

    void deleteOldestMailForNode(uint32_t nodeNum) {
        char dirPath[64]; getMailDirPath(nodeNum, dirPath, sizeof(dirPath));
        uint32_t oldestId = UINT32_MAX;
        char oldestPath[80] = {0};
        File dir = FSCom.open(dirPath, FILE_O_READ);
        if (!dir) return;
        File f;
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
        if (oldestId != UINT32_MAX && oldestPath[0]) FSCom.remove(oldestPath);
    }

    void deleteOldestQSL() {
        uint32_t oldestId = UINT32_MAX;
        char oldestPath[64] = {0};
        File dir = FSCom.open(BBS_QSL_PATH, FILE_O_READ);
        if (!dir) return;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                uint32_t id = strtoul(f.name(), nullptr, 10);
                if (id < oldestId) {
                    oldestId = id;
                    snprintf(oldestPath, sizeof(oldestPath), "%s/%s", BBS_QSL_PATH, f.name());
                }
            }
            f.close();
        }
        dir.close();
        if (oldestId != UINT32_MAX && oldestPath[0]) FSCom.remove(oldestPath);
    }

  public:
    BBSStorageLittleFS() {}
    ~BBSStorageLittleFS() {}

    bool init() override {
        if (!FSCom.exists("/")) {
            printf("[BBS-FS] ERROR: filesystem not mounted\n"); fflush(stdout);
            return false;
        }
        printf("[BBS-FS] FSCom accessible\n"); fflush(stdout);
        if (!ensureDir(BBS_BASE_PATH) || !ensureDir(BBS_BULLETIN_PATH) ||
            !ensureDir(BBS_MAIL_PATH) || !ensureDir(BBS_QSL_PATH) || !ensureDir(BBS_WORDLE_PATH)) {
            printf("[BBS-FS] ERROR: mkdir failed\n"); fflush(stdout);
            return false;
        }
        if (!loadMetadata()) {
            nextBulletinId_ = 1; nextMailId_ = 1; nextQSLId_ = 1;
            saveMetadata();
        }
        initialized_ = true;
        printf("[BBS-FS] Init OK\n"); fflush(stdout);
        return true;
    }

    // ── BULLETINS ──────────────────────────────────────────────────────────

    bool storeBulletin(const BBSBulletin &bulletin) override {
        printf("[BULLETIN] store id=%u board=%u\n", bulletin.id, bulletin.board); fflush(stdout);
        if (!initialized_) return false;
        if (!ensureDir(BBS_BULLETIN_PATH)) return false;

        if (countFiles(BBS_BULLETIN_PATH) >= FLASH_MAX_BULLETINS) {
            deleteOldestBulletin();
        }

        char path[64]; getBulletinPath(bulletin.id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_WRITE);
        if (!f) { printf("[BULLETIN] ERROR: open failed %s\n", path); fflush(stdout); return false; }

        f.write((const uint8_t *)&bulletin.id, sizeof(uint32_t));
        f.write((const uint8_t *)&bulletin.authorNode, sizeof(uint32_t));
        f.write((const uint8_t *)bulletin.authorName, BBS_SHORT_NAME_LEN);
        f.write((const uint8_t *)&bulletin.timestamp, sizeof(uint32_t));
        f.write((const uint8_t *)bulletin.body, BBS_MSG_MAX_LEN + 1);
        f.write((const uint8_t *)&bulletin.board, sizeof(uint8_t)); // appended last
        f.close();

        saveMetadata();
        return true;
    }

    bool loadBulletin(uint32_t id, BBSBulletin &bulletin) override {
        if (!initialized_) return false;
        char path[64]; getBulletinPath(id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_READ);
        if (!f) return false;

        f.read((uint8_t *)&bulletin.id, sizeof(uint32_t));
        f.read((uint8_t *)&bulletin.authorNode, sizeof(uint32_t));
        f.read((uint8_t *)bulletin.authorName, BBS_SHORT_NAME_LEN);
        f.read((uint8_t *)&bulletin.timestamp, sizeof(uint32_t));
        f.read((uint8_t *)bulletin.body, BBS_MSG_MAX_LEN + 1);
        bulletin.board = BOARD_GENERAL; // default if old file (no board byte)
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
        return FSCom.remove(path);
    }

    uint32_t listBulletins(BBSBulletinHeader *headers, uint32_t maxResults, uint32_t offset = 0, uint8_t board = BOARD_ALL) override {
        if (!headers || maxResults == 0 || !initialized_) return 0;

        File dir = FSCom.open(BBS_BULLETIN_PATH, FILE_O_READ);
        if (!dir) return 0;

        uint32_t ids[FLASH_MAX_BULLETINS];
        uint32_t idCount = 0;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory() && idCount < FLASH_MAX_BULLETINS) {
                uint32_t id = 0;
                sscanf(f.name(), "%04" PRIu32, &id);
                ids[idCount++] = id;
            }
            f.close();
        }
        dir.close();

        // Sort descending
        for (uint32_t i = 0; i < idCount - 1; i++)
            for (uint32_t j = i + 1; j < idCount; j++)
                if (ids[i] < ids[j]) { uint32_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }

        uint32_t result = 0;
        uint32_t skipped = 0;
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
        if (board == BOARD_ALL) return countFiles(BBS_BULLETIN_PATH);
        // Count per board by scanning
        BBSBulletinHeader headers[FLASH_MAX_BULLETINS];
        return listBulletins(headers, FLASH_MAX_BULLETINS, 0, board);
    }

    // ── MAIL ───────────────────────────────────────────────────────────────

    bool storeMail(const BBSMailMsg &msg) override {
        if (!initialized_) return false;
        char dirPath[64]; getMailDirPath(msg.toNode, dirPath, sizeof(dirPath));
        if (countFiles(dirPath) >= FLASH_MAX_MAIL_PER_USER) deleteOldestMailForNode(msg.toNode);
        if (!ensureDir(dirPath)) return false;

        char path[80]; getMailPath(msg.toNode, msg.id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_WRITE);
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
        File f = FSCom.open(path, FILE_O_READ);
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
        return FSCom.remove(path);
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
        File dir = FSCom.open(dirPath, FILE_O_READ);
        if (!dir) return 0;

        uint32_t ids[FLASH_MAX_MAIL_PER_USER];
        uint32_t idCount = 0;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory() && idCount < FLASH_MAX_MAIL_PER_USER) {
                uint32_t id = 0; sscanf(f.name(), "%04" PRIu32, &id);
                ids[idCount++] = id;
            }
            f.close();
        }
        dir.close();

        for (uint32_t i = 0; i < idCount - 1; i++)
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
        File dir = FSCom.open(dirPath, FILE_O_READ);
        if (!dir) return 0;
        uint32_t count = 0;
        File f;
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
        if (countFiles(BBS_QSL_PATH) >= FLASH_MAX_QSL) deleteOldestQSL();
        char path[64]; getQSLPath(qsl.id, path, sizeof(path));
        File f = FSCom.open(path, FILE_O_WRITE);
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
        File dir = FSCom.open(BBS_QSL_PATH, FILE_O_READ);
        if (!dir) return 0;
        uint32_t ids[FLASH_MAX_QSL];
        uint32_t idCount = 0;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory() && idCount < FLASH_MAX_QSL) {
                uint32_t id = 0; sscanf(f.name(), "%04" PRIu32, &id); ids[idCount++] = id;
            }
            f.close();
        }
        dir.close();
        for (uint32_t i = 0; i < idCount - 1; i++)
            for (uint32_t j = i + 1; j < idCount; j++)
                if (ids[i] < ids[j]) { uint32_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
        uint32_t result = 0;
        for (uint32_t i = offset; i < idCount && result < maxResults; i++, result++) {
            char path[64]; getQSLPath(ids[i], path, sizeof(path));
            File qf = FSCom.open(path, FILE_O_READ);
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
        return countFiles(BBS_QSL_PATH);
    }

    uint32_t pruneExpiredQSL(uint32_t currentTime) override {
        if (!initialized_) return 0;
        File dir = FSCom.open(BBS_QSL_PATH, FILE_O_READ);
        if (!dir) return 0;
        uint32_t pruned = 0;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                uint32_t fileId = 0; sscanf(f.name(), "%04" PRIu32, &fileId);
                char path[64]; getQSLPath(fileId, path, sizeof(path));
                File qf = FSCom.open(path, FILE_O_READ);
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
                        FSCom.remove(path); pruned++;
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
        stats.freeBytesEstimate = FSCom.totalBytes() - FSCom.usedBytes();
        stats.totalBulletins = totalActiveBulletins();
        stats.totalMailItems = 0;
        stats.totalQSLItems = totalActiveQSL();
        stats.maxBulletins = FLASH_MAX_BULLETINS;
        stats.maxMailPerUser = FLASH_MAX_MAIL_PER_USER;
        return stats;
    }

    uint32_t nextBulletinId() override { uint32_t id = nextBulletinId_++; saveMetadata(); return id; }
    uint32_t nextMailId() override { uint32_t id = nextMailId_++; saveMetadata(); return id; }

};
