#pragma once
// BBSBleUpload — BLE file upload service using Adafruit Bluefruit (nRF52)
//
// Adds a custom BLE service for uploading data to external QSPI flash.
// Works alongside Meshtastic's existing BLE services.
//
// Protocol (over BLE GATT writes):
//   "UPLOAD /bbs/kb/geo_us.bin 378660\n" → "READY\n"
//   <binary data in chunks>
//   "DONE\n" → "OK 378660\n"
//   "LIST\n" → file listing
//   "WIPE\n" → wipe /bbs/kb/

#if defined(NRF52_SERIES)

#include <bluefruit.h>
#include "BBSExtFlash.h"
#include "DebugConfiguration.h"

// Custom 128-bit UUIDs for BBS upload service
static const uint8_t BBS_UPLOAD_SERVICE_UUID[] = {
    0x01, 0x00, 0xB1, 0xBB, 0x1E, 0xBB, 0x96, 0x83,
    0x52, 0x4F, 0x74, 0x73, 0x00, 0xBB, 0x1B, 0x7B
};
static const uint8_t BBS_UPLOAD_RX_UUID[] = {
    0x01, 0x00, 0xB1, 0xBB, 0x1E, 0xBB, 0x96, 0x83,
    0x52, 0x4F, 0x74, 0x73, 0x01, 0xBB, 0x1B, 0x7B
};
static const uint8_t BBS_UPLOAD_TX_UUID[] = {
    0x01, 0x00, 0xB1, 0xBB, 0x1E, 0xBB, 0x96, 0x83,
    0x52, 0x4F, 0x74, 0x73, 0x02, 0xBB, 0x1B, 0x7B
};

class BBSBleUpload {
  private:
    BLEService        service_;
    BLECharacteristic rxChar_;
    BLECharacteristic txChar_;

    File    *uploadFile_ = nullptr;  // heap-allocated to avoid global constructor
    uint32_t expectedSize_ = 0;
    uint32_t receivedSize_ = 0;
    bool     uploading_ = false;
    char     uploadPath_[64] = {0};

    void sendResponse(const char *msg) {
        if (Bluefruit.connected()) {
            txChar_.notify((const uint8_t *)msg, strlen(msg));
        }
    }

    void handleCommand(const uint8_t *data, uint16_t len) {
        char cmd[256];
        uint16_t cmdLen = (len < 255) ? len : 255;
        memcpy(cmd, data, cmdLen);
        cmd[cmdLen] = '\0';
        while (cmdLen > 0 && (cmd[cmdLen-1] == '\n' || cmd[cmdLen-1] == '\r'))
            cmd[--cmdLen] = '\0';

        if (strncmp(cmd, "UPLOAD ", 7) == 0) {
            char path[64] = {0};
            uint32_t size = 0;
            if (sscanf(cmd + 7, "%63s %u", path, &size) != 2 || size == 0) {
                sendResponse("ERR bad args\n");
                return;
            }
            if (!bbsExtFS().exists("/bbs")) bbsExtFS().mkdir("/bbs");
            if (!bbsExtFS().exists("/bbs/kb")) bbsExtFS().mkdir("/bbs/kb");
            if (bbsExtFS().exists(path)) bbsExtFS().remove(path);

            if (uploadFile_) { delete uploadFile_; uploadFile_ = nullptr; }
            File f = bbsExtFS().open(path, FILE_O_WRITE);
            if (!f) { sendResponse("ERR open\n"); return; }
            uploadFile_ = new File(f);

            strncpy(uploadPath_, path, sizeof(uploadPath_) - 1);
            expectedSize_ = size;
            receivedSize_ = 0;
            uploading_ = true;
            LOG_INFO("[BBS-BLE] Upload: %s (%u bytes)\n", path, size);
            sendResponse("READY\n");
            return;
        }

        if (strncmp(cmd, "DONE", 4) == 0) {
            finishUpload();
            return;
        }

        if (strncmp(cmd, "LIST", 4) == 0) {
            File dir = bbsExtFS().open("/bbs/kb", FILE_O_READ);
            if (!dir) { sendResponse("(empty)\n"); return; }
            File f(bbsExtFS());
            char line[48];
            while ((f = dir.openNextFile())) {
                snprintf(line, sizeof(line), "%s %u\n", f.name(), (uint32_t)f.size());
                sendResponse(line);
                f.close();
            }
            dir.close();
            sendResponse("END\n");
            return;
        }

        if (strncmp(cmd, "WIPE", 4) == 0) {
            File dir = bbsExtFS().open("/bbs/kb", FILE_O_READ);
            if (dir) {
                File f(bbsExtFS());
                char names[32][32];
                int count = 0;
                while ((f = dir.openNextFile()) && count < 32) {
                    strncpy(names[count], f.name(), 31);
                    names[count][31] = '\0';
                    count++;
                    f.close();
                }
                dir.close();
                for (int i = 0; i < count; i++) {
                    char fp[64];
                    snprintf(fp, sizeof(fp), "/bbs/kb/%s", names[i]);
                    bbsExtFS().remove(fp);
                }
            }
            sendResponse("WIPED\n");
            return;
        }

        sendResponse("ERR unknown\n");
    }

    void finishUpload() {
        if (!uploading_) return;
        if (uploadFile_) { uploadFile_->close(); delete uploadFile_; uploadFile_ = nullptr; }
        uploading_ = false;
        char resp[48];
        snprintf(resp, sizeof(resp), "OK %u\n", receivedSize_);
        LOG_INFO("[BBS-BLE] Done: %u bytes\n", receivedSize_);
        sendResponse(resp);
    }

    // Static callback — Bluefruit requires a C-style function pointer
    static BBSBleUpload *instance_;

    static void onRxWrite(uint16_t conn_hdl, BLECharacteristic *chr, uint8_t *data, uint16_t len) {
        (void)conn_hdl;
        (void)chr;
        if (!instance_) return;

        if (instance_->uploading_) {
            // Check for DONE command
            if (len <= 6 && len >= 4 && strncmp((const char *)data, "DONE", 4) == 0) {
                instance_->finishUpload();
                return;
            }
            if (instance_->uploadFile_) instance_->uploadFile_->write(data, len);
            instance_->receivedSize_ += len;
        } else {
            instance_->handleCommand(data, len);
        }
    }

  public:
    BBSBleUpload() : service_(BBS_UPLOAD_SERVICE_UUID),
                     rxChar_(BBS_UPLOAD_RX_UUID),
                     txChar_(BBS_UPLOAD_TX_UUID) {}

    bool begin() {
        instance_ = this;

        service_.begin();

        // RX: phone writes data/commands here
        rxChar_.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
        rxChar_.setPermission(SECMODE_OPEN, SECMODE_OPEN);
        rxChar_.setMaxLen(512);
        rxChar_.setWriteCallback(onRxWrite);
        rxChar_.begin();

        // TX: device notifies phone here
        txChar_.setProperties(CHR_PROPS_NOTIFY);
        txChar_.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
        txChar_.setMaxLen(64);
        txChar_.begin();

        // Add to advertising and restart it so the new service is discoverable
        Bluefruit.Advertising.addService(service_);
        Bluefruit.Advertising.restartOnDisconnect(true);
        if (Bluefruit.Advertising.isRunning()) {
            Bluefruit.Advertising.stop();
            Bluefruit.Advertising.start(0);
        }

        LOG_INFO("[BBS-BLE] Upload service ready\n");
        return true;
    }
};

BBSBleUpload *BBSBleUpload::instance_ = nullptr;

#endif // NRF52_SERIES
