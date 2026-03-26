/*
 * TinyBBS Knowledge Base Serial Loader
 *
 * Standalone firmware — no Meshtastic, just QSPI flash + serial.
 * Receives files over serial and writes them to external flash.
 *
 * Protocol (same 0xBB framing):
 *   Frame: 0xBB <cmd:1> <len:2 LE> <data:len> <crc8:1>
 *   Response: 0xBB 0x80 <status:1> (0=OK, 1=ERR)
 *   Commands: 0x01=OPEN, 0x02=DATA, 0x03=CLOSE, 0x04=LIST, 0x05=WIPE
 *
 * Flash this, upload files via serial_upload.py, then flash BBS firmware.
 */

#include <Arduino.h>
#include "BBSExtFlash.h"

using namespace Adafruit_LittleFS_Namespace;

#define LED_PIN LED_BUILTIN

static File *uploadFile = nullptr;
static uint32_t expectedSize = 0;
static uint32_t receivedSize = 0;

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

static void sendResponse(uint8_t status) {
    uint8_t resp[3] = {0xBB, 0x80, status};
    Serial.write(resp, 3);
    Serial.flush();
}

static void handleOpen(const uint8_t *data, uint16_t len) {
    if (len < 6) { sendResponse(1); return; }

    uint8_t pathLen = data[0];
    if (pathLen + 5 > len) { sendResponse(1); return; }

    char path[64] = {0};
    memcpy(path, data + 1, pathLen < 63 ? pathLen : 63);

    uint32_t fileSize;
    memcpy(&fileSize, data + 1 + pathLen, 4);

    // Ensure directories
    const char *dirs[] = {"/bbs", "/bbs/kb", "/bbs/bul", "/bbs/mail", "/bbs/qsl", "/bbs/wdl", "/bbs/frpg",
                          "/clique", "/clique/dmq"};
    for (const char *d : dirs) {
        if (!bbsExtFS().exists(d)) bbsExtFS().mkdir(d);
    }

    // Remove old file
    if (bbsExtFS().exists(path)) bbsExtFS().remove(path);

    if (uploadFile) { uploadFile->close(); delete uploadFile; uploadFile = nullptr; }

    File f = bbsExtFS().open(path, FILE_O_WRITE);
    if (!f) {
        Serial.print("ERR: Can't open "); Serial.println(path);
        sendResponse(1);
        return;
    }

    uploadFile = new File(f);
    expectedSize = fileSize;
    receivedSize = 0;

    Serial.print("OPEN: "); Serial.print(path);
    Serial.print(" ("); Serial.print(fileSize); Serial.println(" bytes)");
    sendResponse(0);
}

static void handleData(const uint8_t *data, uint16_t len) {
    if (!uploadFile) { sendResponse(1); return; }

    // Copy through RAM buffer (required for QSPI writes from flash-mapped data)
    uint8_t ramBuf[256];
    uint16_t pos = 0;
    while (pos < len) {
        uint16_t chunk = len - pos;
        if (chunk > sizeof(ramBuf)) chunk = sizeof(ramBuf);
        memcpy(ramBuf, data + pos, chunk);
        uploadFile->write(ramBuf, chunk);
        pos += chunk;
    }

    receivedSize += len;
    sendResponse(0);
}

static void handleClose() {
    if (uploadFile) {
        uploadFile->close();
        delete uploadFile;
        uploadFile = nullptr;
        Serial.print("CLOSE: "); Serial.print(receivedSize);
        Serial.print("/"); Serial.print(expectedSize); Serial.println(" bytes");
    }
    sendResponse(0);
}

static void handleList() {
    const char *dirs[] = {"/bbs/kb"};
    for (const char *dirPath : dirs) {
        File dir = bbsExtFS().open(dirPath, FILE_O_READ);
        if (!dir) continue;
        File f(bbsExtFS());
        while ((f = dir.openNextFile())) {
            Serial.print("  "); Serial.print(dirPath); Serial.print("/");
            Serial.print(f.name()); Serial.print("  ");
            Serial.print((uint32_t)f.size()); Serial.println(" bytes");
            f.close();
        }
        dir.close();
    }

    uint32_t used = bbsExtFS().usedBytes();
    uint32_t total = bbsExtFS().totalBytes();
    Serial.print("Storage: "); Serial.print(used/1024); Serial.print("KB / ");
    Serial.print(total/1024); Serial.println("KB");

    sendResponse(0);
}

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== TinyBBS Serial Loader ===");
    Serial.println("Waiting for upload commands...");
    Serial.println("Use: python3 scripts/serial_upload.py upload <file> <path>");
    Serial.println();

    if (!bbsExtFS().begin()) {
        Serial.println("FATAL: QSPI flash init failed!");
        while (1) { digitalWrite(LED_PIN, HIGH); delay(1000); digitalWrite(LED_PIN, LOW); delay(1000); }
    }
    Serial.println("External flash ready (2MB)");

    // Show current files
    handleList();

    Serial.println();
    Serial.println("Ready for uploads. Send 0xBB frames.");
    digitalWrite(LED_PIN, LOW);
}

void loop() {
    if (Serial.available() < 4) return;

    if (Serial.peek() != 0xBB) {
        Serial.read(); // discard non-frame bytes
        return;
    }

    // Read frame header
    uint8_t hdr[4];
    Serial.readBytes(hdr, 4);

    uint8_t cmd = hdr[1];
    uint16_t dataLen = hdr[2] | (hdr[3] << 8);

    if (dataLen > 512) {
        Serial.println("ERR: frame too large");
        return;
    }

    // Read payload + CRC
    uint8_t buf[514];
    if (dataLen + 1 > 0) {
        size_t got = Serial.readBytes(buf, dataLen + 1);
        if (got != (size_t)(dataLen + 1)) {
            Serial.println("ERR: timeout reading payload");
            return;
        }
    }

    // Verify CRC
    // Build full frame for CRC: hdr(4) + payload(dataLen)
    uint8_t crcBuf[518];
    memcpy(crcBuf, hdr, 4);
    memcpy(crcBuf + 4, buf, dataLen);
    uint8_t expected = buf[dataLen];
    uint8_t actual = crc8(crcBuf, 4 + dataLen);

    if (expected != actual) {
        Serial.println("ERR: bad CRC");
        sendResponse(1);
        return;
    }

    // Toggle LED on activity
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));

    switch (cmd) {
        case 0x01: handleOpen(buf, dataLen); break;
        case 0x02: handleData(buf, dataLen); break;
        case 0x03: handleClose(); break;
        case 0x04: handleList(); break;
        default: sendResponse(1); break;
    }
}
