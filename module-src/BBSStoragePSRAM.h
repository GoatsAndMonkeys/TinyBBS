#pragma once

#include "BBSStorage.h"
#include "DebugConfiguration.h"
#include <cstring>

#ifdef ARCH_ESP32
#include <esp_heap_caps.h>
#endif

/**
 * PSRAM-based storage for ESP32 boards with PSRAM (e.g., Heltec LoRa 32 V3)
 * Uses ps_malloc() for fast volatile in-memory storage.
 * Data is lost on reboot.
 */
class BBSStoragePSRAM : public BBSStorage {
  private:
    BBSBulletin *bulletins_ = nullptr;
    uint32_t bulletinCapacity_ = PSRAM_MAX_BULLETINS;
    uint32_t bulletinCount_ = 0;

    BBSMailMsg *mail_ = nullptr;
    uint32_t mailCapacity_ = PSRAM_MAX_MAILBOXES * PSRAM_MAX_MAIL_PER_USER;
    uint32_t mailCount_ = 0;

    BBSQSL *qsl_ = nullptr;
    uint32_t qslCapacity_ = PSRAM_MAX_QSL;
    uint32_t qslCount_ = 0;

    uint32_t nextBulletinId_ = 1;
    uint32_t nextMailId_ = 1;
    uint32_t nextQSLId_ = 1;


    /**
     * Compare function for sorting bulletins by ID descending
     */
    static int compare_bulletin_id_desc(const void *a, const void *b) {
        const BBSBulletin *ba = (const BBSBulletin *)a;
        const BBSBulletin *bb = (const BBSBulletin *)b;
        if (ba->id == bb->id) return 0;
        return (ba->id > bb->id) ? -1 : 1;
    }

  public:
    BBSStoragePSRAM() {}

    ~BBSStoragePSRAM() {
        if (bulletins_) {
            free(bulletins_);
            bulletins_ = nullptr;
        }
        if (mail_) {
            free(mail_);
            mail_ = nullptr;
        }
        if (qsl_) {
            free(qsl_);
            qsl_ = nullptr;
        }
    }

    bool init() override {
        LOG_DEBUG("[STORAGE] Initializing PSRAM storage");

        // Reset counters
        bulletinCount_ = 0;
        mailCount_ = 0;
        qslCount_ = 0;
        nextBulletinId_ = 1;
        nextMailId_ = 1;
        nextQSLId_ = 1;

        // Allocate bulletin array
#ifdef ARCH_ESP32
        LOG_DEBUG("[STORAGE] Allocating bulletins: %u * %zu bytes", bulletinCapacity_, sizeof(BBSBulletin));
        bulletins_ = (BBSBulletin *)ps_calloc(bulletinCapacity_, sizeof(BBSBulletin));
        if (!bulletins_) {
            LOG_ERROR("[STORAGE] ERROR: Failed to allocate bulletins");
            return false;
        }

        // Allocate flat mail array
        LOG_DEBUG("[STORAGE] Allocating mail: %u * %zu bytes", mailCapacity_, sizeof(BBSMailMsg));
        mail_ = (BBSMailMsg *)ps_calloc(mailCapacity_, sizeof(BBSMailMsg));
        if (!mail_) {
            LOG_ERROR("[STORAGE] ERROR: Failed to allocate mail");
            free(bulletins_);
            bulletins_ = nullptr;
            return false;
        }

        // Allocate QSL array
        LOG_DEBUG("[STORAGE] Allocating QSL: %u * %zu bytes", qslCapacity_, sizeof(BBSQSL));
        qsl_ = (BBSQSL *)ps_calloc(qslCapacity_, sizeof(BBSQSL));
        if (!qsl_) {
            LOG_ERROR("[STORAGE] ERROR: Failed to allocate QSL");
            free(bulletins_);
            free(mail_);
            bulletins_ = nullptr;
            mail_ = nullptr;
            return false;
        }
#else
        // Fallback for non-ESP32 platforms during testing
        bulletins_ = (BBSBulletin *)calloc(bulletinCapacity_, sizeof(BBSBulletin));
        if (!bulletins_) {
            return false;
        }
        mail_ = (BBSMailMsg *)calloc(mailCapacity_, sizeof(BBSMailMsg));
        if (!mail_) {
            free(bulletins_);
            bulletins_ = nullptr;
            return false;
        }
        qsl_ = (BBSQSL *)calloc(qslCapacity_, sizeof(BBSQSL));
        if (!qsl_) {
            free(bulletins_);
            free(mail_);
            bulletins_ = nullptr;
            mail_ = nullptr;
            return false;
        }
#endif
        LOG_DEBUG("[STORAGE] Init complete - Bulletins: %u, Mail: %u, QSL: %u",
                  bulletinCount_, mailCount_, qslCount_);
        return true;
    }

    bool storeBulletin(const BBSBulletin &bulletin) override {
        if (!bulletins_) {
            return false;
        }

        // If full, shift old entries and make room
        if (bulletinCount_ >= bulletinCapacity_) {
            for (uint32_t i = 0; i < bulletinCount_ - 1; i++) {
                bulletins_[i] = bulletins_[i + 1];
            }
            bulletinCount_--;
        }

        bulletins_[bulletinCount_] = bulletin;
        bulletins_[bulletinCount_].active = true;
        bulletinCount_++;
        return true;
    }

    bool loadBulletin(uint32_t id, BBSBulletin &bulletin) override {
        for (uint32_t i = 0; i < bulletinCount_; i++) {
            if (bulletins_[i].active && bulletins_[i].id == id) {
                bulletin = bulletins_[i];
                return true;
            }
        }
        return false;
    }

    bool deleteBulletin(uint32_t id, uint32_t requestorNodeNum) override {
        for (uint32_t i = 0; i < bulletinCount_; i++) {
            if (bulletins_[i].active && bulletins_[i].id == id) {
                // Only author can delete
                if (bulletins_[i].authorNode != requestorNodeNum) {
                    return false;
                }
                bulletins_[i].active = false;
                return true;
            }
        }
        return false;
    }

    uint32_t listBulletins(BBSBulletinHeader *headers, uint32_t maxResults, uint32_t offset = 0, uint8_t board = BOARD_ALL) override {
        if (!headers || maxResults == 0) {
            return 0;
        }

        // Collect active bulletins matching board filter
        BBSBulletin temp[PSRAM_MAX_BULLETINS];
        uint32_t activeCount = 0;

        for (uint32_t i = 0; i < bulletinCount_; i++) {
            if (bulletins_[i].active && (board == BOARD_ALL || bulletins_[i].board == board)) {
                temp[activeCount++] = bulletins_[i];
            }
        }

        // Sort by ID descending
        qsort(temp, activeCount, sizeof(BBSBulletin), compare_bulletin_id_desc);

        // Return requested page
        uint32_t result = 0;
        for (uint32_t i = offset; i < activeCount && result < maxResults; i++, result++) {
            headers[result].id = temp[i].id;
            headers[result].authorNode = temp[i].authorNode;
            strncpy(headers[result].authorName, temp[i].authorName, BBS_SHORT_NAME_LEN - 1);
            headers[result].authorName[BBS_SHORT_NAME_LEN - 1] = '\0';
            headers[result].timestamp = temp[i].timestamp;
            strncpy(headers[result].preview, temp[i].body, BBS_PREVIEW_LEN);
            headers[result].preview[BBS_PREVIEW_LEN] = '\0';
            headers[result].board = temp[i].board;
        }

        return result;
    }

    uint32_t totalActiveBulletins(uint8_t board = BOARD_ALL) override {
        uint32_t count = 0;
        for (uint32_t i = 0; i < bulletinCount_; i++) {
            if (bulletins_[i].active && (board == BOARD_ALL || bulletins_[i].board == board)) {
                count++;
            }
        }
        return count;
    }

    bool storeMail(const BBSMailMsg &msg) override {
        if (!mail_) {
            return false;
        }

        // Check per-user limit - if exceeded, remove oldest for that user
        uint32_t userCount = 0;
        uint32_t oldestIdx = UINT32_MAX;
        for (uint32_t i = 0; i < mailCount_; i++) {
            if (mail_[i].active && mail_[i].toNode == msg.toNode) {
                userCount++;
                if (oldestIdx == UINT32_MAX) {
                    oldestIdx = i;
                }
            }
        }
        if (userCount >= PSRAM_MAX_MAIL_PER_USER && oldestIdx != UINT32_MAX) {
            // Remove oldest mail for this user
            mail_[oldestIdx].active = false;
            userCount--;
        }

        // If storage full, shift old entries
        if (mailCount_ >= mailCapacity_) {
            for (uint32_t i = 0; i < mailCount_ - 1; i++) {
                mail_[i] = mail_[i + 1];
            }
            mailCount_--;
        }

        mail_[mailCount_] = msg;
        mail_[mailCount_].active = true;
        mail_[mailCount_].read = false;
        mailCount_++;
        return true;
    }

    bool loadMail(uint32_t id, uint32_t recipientNode, BBSMailMsg &msg) override {
        for (uint32_t i = 0; i < mailCount_; i++) {
            if (mail_[i].active && mail_[i].id == id && mail_[i].toNode == recipientNode) {
                msg = mail_[i];
                return true;
            }
        }
        return false;
    }

    bool deleteMail(uint32_t id, uint32_t recipientNode) override {
        for (uint32_t i = 0; i < mailCount_; i++) {
            if (mail_[i].active && mail_[i].id == id && mail_[i].toNode == recipientNode) {
                mail_[i].active = false;
                return true;
            }
        }
        return false;
    }

    bool markMailRead(uint32_t id, uint32_t recipientNode) override {
        for (uint32_t i = 0; i < mailCount_; i++) {
            if (mail_[i].active && mail_[i].id == id && mail_[i].toNode == recipientNode) {
                mail_[i].read = true;
                return true;
            }
        }
        return false;
    }

    uint32_t listMail(uint32_t recipientNode, BBSMailHeader *headers, uint32_t maxResults, uint32_t offset = 0) override {
        if (!headers || maxResults == 0) {
            return 0;
        }

        uint32_t result = 0;
        uint32_t skipped = 0;

        // Iterate through mail, collecting headers for this recipient
        // Sort by ID descending (newest first)
        for (uint32_t i = 0; i < mailCount_; i++) {
            if (mail_[i].active && mail_[i].toNode == recipientNode) {
                if (skipped < offset) {
                    skipped++;
                    continue;
                }
                if (result >= maxResults) {
                    break;
                }

                headers[result].id = mail_[i].id;
                headers[result].fromNode = mail_[i].fromNode;
                strncpy(headers[result].fromName, mail_[i].fromName, BBS_SHORT_NAME_LEN - 1);
                headers[result].fromName[BBS_SHORT_NAME_LEN - 1] = '\0';
                headers[result].timestamp = mail_[i].timestamp;
                strncpy(headers[result].subject, mail_[i].subject, BBS_SUBJECT_LEN - 1);
                headers[result].subject[BBS_SUBJECT_LEN - 1] = '\0';
                headers[result].read = mail_[i].read;
                result++;
            }
        }

        return result;
    }

    uint32_t countUnreadMail(uint32_t recipientNode) override {
        uint32_t count = 0;
        for (uint32_t i = 0; i < mailCount_; i++) {
            if (mail_[i].active && mail_[i].toNode == recipientNode && !mail_[i].read) {
                count++;
            }
        }
        return count;
    }

    bool storeQSL(const BBSQSL &qsl) override {
        if (!qsl_) {
            return false;
        }

        // If full, remove oldest entry and shift others
        if (qslCount_ >= qslCapacity_) {
            // Shift all entries down by 1
            for (uint32_t i = 0; i < qslCount_ - 1; i++) {
                qsl_[i] = qsl_[i + 1];
            }
            qslCount_--;
        }

        // Add new entry at the end
        qsl_[qslCount_] = qsl;
        qsl_[qslCount_].active = true;
        qslCount_++;
        return true;
    }

    uint32_t listQSL(BBSQSLHeader *headers, uint32_t maxResults, uint32_t offset = 0) override {
        if (!headers || maxResults == 0) {
            return 0;
        }

        // Create temporary array of active QSL entries for sorting
        BBSQSL temp[PSRAM_MAX_QSL];
        uint32_t activeCount = 0;

        for (uint32_t i = 0; i < qslCount_; i++) {
            if (qsl_[i].active) {
                temp[activeCount++] = qsl_[i];
            }
        }

        // Sort by ID descending (newest first)
        for (uint32_t i = 0; i < activeCount - 1; i++) {
            for (uint32_t j = i + 1; j < activeCount; j++) {
                if (temp[i].id < temp[j].id) {
                    BBSQSL t = temp[i];
                    temp[i] = temp[j];
                    temp[j] = t;
                }
            }
        }

        // Return requested page
        uint32_t result = 0;
        for (uint32_t i = offset; i < activeCount && result < maxResults; i++, result++) {
            headers[result].id = temp[i].id;
            headers[result].fromNode = temp[i].fromNode;
            strncpy(headers[result].fromName, temp[i].fromName, BBS_SHORT_NAME_LEN - 1);
            headers[result].fromName[BBS_SHORT_NAME_LEN - 1] = '\0';
            headers[result].timestamp = temp[i].timestamp;
            headers[result].hopsAway = temp[i].hopsAway;
            headers[result].hasLocation = (temp[i].latitude != 0 || temp[i].longitude != 0);
            strncpy(headers[result].location, temp[i].location, sizeof(headers[result].location) - 1);
            headers[result].location[sizeof(headers[result].location) - 1] = '\0';
        }

        return result;
    }

    uint32_t totalActiveQSL() override {
        uint32_t count = 0;
        for (uint32_t i = 0; i < qslCount_; i++) {
            if (qsl_[i].active) {
                count++;
            }
        }
        return count;
    }

    uint32_t pruneExpiredQSL(uint32_t currentTime) override {
        uint32_t pruned = 0;
        for (uint32_t i = 0; i < qslCount_; i++) {
            if (qsl_[i].active && (currentTime - qsl_[i].timestamp) > QSL_TTL_SECONDS) {
                qsl_[i].active = false;
                pruned++;
            }
        }
        return pruned;
    }

    uint32_t nextQSLId() override { return nextQSLId_++; }

    bool compact() override {
        // Shift active bulletins to front
        uint32_t writeIdx = 0;
        for (uint32_t i = 0; i < bulletinCount_; i++) {
            if (bulletins_[i].active) {
                bulletins_[writeIdx] = bulletins_[i];
                writeIdx++;
            }
        }
        bulletinCount_ = writeIdx;

        // Shift active mail to front
        writeIdx = 0;
        for (uint32_t i = 0; i < mailCount_; i++) {
            if (mail_[i].active) {
                mail_[writeIdx] = mail_[i];
                writeIdx++;
            }
        }
        mailCount_ = writeIdx;

        // Shift active QSL to front
        writeIdx = 0;
        for (uint32_t i = 0; i < qslCount_; i++) {
            if (qsl_[i].active) {
                qsl_[writeIdx] = qsl_[i];
                writeIdx++;
            }
        }
        qslCount_ = writeIdx;

        return true;
    }

    BBSStats getStats() override {
        BBSStats stats;
#ifdef ARCH_ESP32
        stats.freeBytesEstimate = ESP.getFreePsram();
#else
        stats.freeBytesEstimate = 0;
#endif
        stats.totalBulletins = totalActiveBulletins();
        stats.totalMailItems = mailCount_;
        stats.totalQSLItems = totalActiveQSL();
        stats.maxBulletins = bulletinCapacity_;
        stats.maxMailPerUser = PSRAM_MAX_MAIL_PER_USER;
        return stats;
    }

    uint32_t nextBulletinId() override { return nextBulletinId_++; }

    uint32_t nextMailId() override { return nextMailId_++; }

};
