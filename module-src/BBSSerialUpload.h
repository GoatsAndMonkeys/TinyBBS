#pragma once
// BBSSerialUpload — File upload via serial using custom framing
//
// Uses 0xBB as frame header — Meshtastic's serial parser only looks for
// 0x94C3, so our frames are invisible to it. No conflict.
//
// Frame format: 0xBB <cmd:1> <len:2 LE> <data:len> <crc8:1>
// Commands: 0x01=OPEN, 0x02=DATA, 0x03=CLOSE
// Response: 0xBB 0x80 <status:1> (0=OK, 1=ERR)

#ifdef NRF52_SERIES

#include "BBSExtFlash.h"
#include "DebugConfiguration.h"

class BBSSerialUpload {
  private:
    File uploadFile_ = File(bbsExtFS());
    uint32_t expectedSize_ = 0;
    uint32_t receivedSize_ = 0;
    bool uploading_ = false;

    static uint8_t crc8(const uint8_t *data, size_t len) {
        uint8_t crc = 0;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int j = 0; j < 8; j++) {
                if (crc & 0x80) crc = ((crc << 1) ^ 0x07);
                else crc = (crc << 1);
            }
        }
        return crc;
    }

    void sendResponse(uint8_t status) {
        uint8_t resp[3] = {0xBB, 0x80, status};
        Serial.write(resp, 3);
    }

    void handleOpen(const uint8_t *data, uint16_t len) {
        if (len < 6) { sendResponse(1); return; }

        uint8_t pathLen = data[0];
        if (pathLen + 5 > len) { sendResponse(1); return; }

        char path[64] = {0};
        memcpy(path, data + 1, pathLen < 63 ? pathLen : 63);

        uint32_t fileSize;
        memcpy(&fileSize, data + 1 + pathLen, 4);

        // Ensure directories exist
        if (!bbsExtFS().exists("/bbs")) bbsExtFS().mkdir("/bbs");
        if (!bbsExtFS().exists("/bbs/kb")) bbsExtFS().mkdir("/bbs/kb");

        // Remove old file
        if (bbsExtFS().exists(path)) bbsExtFS().remove(path);

        uploadFile_ = bbsExtFS().open(path, FILE_O_WRITE);
        if (!uploadFile_) {
            LOG_ERROR("[SerUpload] Can't open %s\n", path);
            sendResponse(1);
            return;
        }

        expectedSize_ = fileSize;
        receivedSize_ = 0;
        uploading_ = true;
        LOG_INFO("[SerUpload] Open: %s (%u bytes)\n", path, fileSize);
        sendResponse(0);
    }

    void handleData(const uint8_t *data, uint16_t len) {
        if (!uploading_) { sendResponse(1); return; }
        uploadFile_.write(data, len);
        receivedSize_ += len;
        sendResponse(0);
    }

    void handleClose() {
        if (uploading_) {
            uploadFile_.close();
            uploading_ = false;
            LOG_INFO("[SerUpload] Done: %u/%u bytes\n", receivedSize_, expectedSize_);
        }
        sendResponse(0);
    }

  public:
    // Call from runOnce() — checks for our 0xBB frames on Serial
    void poll() {
        while (Serial.available() >= 5) {  // minimum frame: BB cmd len(2) crc
            if (Serial.peek() != 0xBB) {
                // Not our frame — leave it for Meshtastic
                return;
            }

            // Read frame header
            uint8_t hdr[4];
            Serial.readBytes(hdr, 4);  // BB cmd lenLo lenHi

            uint8_t cmd = hdr[1];
            uint16_t dataLen = hdr[2] | (hdr[3] << 8);

            if (dataLen > 512) {
                // Corrupt frame, skip
                return;
            }

            // Read payload + CRC
            uint8_t buf[516];
            memcpy(buf, hdr, 4);
            if (dataLen + 1 > 0) {
                size_t got = Serial.readBytes(buf + 4, dataLen + 1);
                if (got != dataLen + 1) {
                    return;  // timeout
                }
            }

            // Verify CRC
            uint8_t expectedCrc = buf[4 + dataLen];
            uint8_t actualCrc = crc8(buf, 4 + dataLen);
            if (expectedCrc != actualCrc) {
                sendResponse(1);
                continue;
            }

            // Dispatch
            switch (cmd) {
                case 0x01: handleOpen(buf + 4, dataLen); break;
                case 0x02: handleData(buf + 4, dataLen); break;
                case 0x03: handleClose(); break;
                default: sendResponse(1); break;
            }
        }
    }
};

#endif // NRF52_SERIES
