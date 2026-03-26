/*
 * TinyBBS Knowledge Base Loader
 *
 * Lightweight firmware that writes data files to the external QSPI flash.
 * Flash this first, let it run (LED blinks when done), then flash the BBS firmware.
 *
 * What it does:
 *   1. Initialize QSPI external flash
 *   2. Mount or format LittleFS on it
 *   3. Create /bbs/ directory structure
 *   4. Wipe old knowledge base files (/bbs/kb/*)
 *   5. Write embedded data files to /bbs/kb/
 *   6. Blink LED rapidly = done, flash BBS firmware now
 *
 * Build: pio run -e t-echo-loader
 * Flash: copy UF2 to TECHOBOOT
 */

#include <Arduino.h>
#include <Wire.h>

// We reuse the BBSExtFlash driver
#include "BBSExtFlash.h"

// Include the knowledge base data headers
#include "kb_geo.h"      // geo city database
// #include "kb_survival.h"  // survival guide (add later)
// #include "kb_dict.h"      // dictionary (add later)

// LED pin (T-Echo green LED)
#ifndef LED_GREEN
#define LED_GREEN LED_BUILTIN
#endif

static void blinkOK() {
    // Rapid blink = success
    for (;;) {
        digitalWrite(LED_GREEN, HIGH);
        delay(100);
        digitalWrite(LED_GREEN, LOW);
        delay(100);
    }
}

static void blinkErr() {
    // Slow blink = error
    for (;;) {
        digitalWrite(LED_GREEN, HIGH);
        delay(1000);
        digitalWrite(LED_GREEN, LOW);
        delay(1000);
    }
}

// Write a data blob to a file on external flash
static bool writeFile(const char *path, const uint8_t *data, uint32_t size) {
    Serial.print("  Writing ");
    Serial.print(path);
    Serial.print(" (");
    Serial.print(size);
    Serial.println(" bytes)...");

    File f = bbsExtFS.open(path, FILE_O_WRITE);
    if (!f) {
        Serial.println("  ERROR: open failed");
        return false;
    }

    // Write in chunks to avoid blocking too long
    uint32_t written = 0;
    while (written < size) {
        uint32_t chunk = size - written;
        if (chunk > 256) chunk = 256;
        f.write(data + written, chunk);
        written += chunk;
    }

    f.close();
    Serial.print("  OK: ");
    Serial.print(written);
    Serial.println(" bytes written");
    return true;
}

// Recursively delete all files in a directory
static void wipeDir(const char *dirPath) {
    File dir = bbsExtFS.open(dirPath, FILE_O_READ);
    if (!dir) return;

    File f(bbsExtFS);
    // Collect filenames first (can't delete while iterating)
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
        char fullPath[64];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, names[i]);
        bbsExtFS.remove(fullPath);
        Serial.print("  Removed: ");
        Serial.println(fullPath);
    }
}

void setup() {
    pinMode(LED_GREEN, OUTPUT);
    digitalWrite(LED_GREEN, HIGH); // LED on during work

    Serial.begin(115200);
    delay(2000); // wait for serial monitor

    Serial.println("========================================");
    Serial.println("  TinyBBS Knowledge Base Loader");
    Serial.println("========================================");
    Serial.println();

    // Step 1: Init external flash
    Serial.println("[1/5] Initializing external QSPI flash...");
    if (!bbsExtFS.begin()) {
        Serial.println("FATAL: External flash init failed!");
        blinkErr();
    }
    Serial.println("  External flash ready (2MB)");
    Serial.println();

    // Step 2: Create directory structure
    Serial.println("[2/5] Creating directory structure...");
    const char *dirs[] = {"/bbs", "/bbs/bul", "/bbs/mail", "/bbs/qsl",
                          "/bbs/wdl", "/bbs/kb", "/bbs/frpg"};
    for (const char *d : dirs) {
        if (!bbsExtFS.exists(d)) {
            bbsExtFS.mkdir(d);
            Serial.print("  Created: ");
        } else {
            Serial.print("  Exists:  ");
        }
        Serial.println(d);
    }
    Serial.println();

    // Step 3: Wipe old knowledge base
    Serial.println("[3/5] Wiping old knowledge base...");
    wipeDir("/bbs/kb");
    Serial.println();

    // Step 4: Write new knowledge base files
    Serial.println("[4/5] Writing knowledge base...");

    // Geo database
    writeFile("/bbs/kb/geo_us.bin", KB_GEO_DATA, KB_GEO_SIZE);

    // Add more files here as they're created:
    // writeFile("/bbs/kb/survival.bin", KB_SURVIVAL_DATA, KB_SURVIVAL_SIZE);
    // writeFile("/bbs/kb/dict_a.bin", KB_DICT_A_DATA, KB_DICT_A_SIZE);

    Serial.println();

    // Step 5: Verify
    Serial.println("[5/5] Verifying...");
    File vf = bbsExtFS.open("/bbs/kb/geo_us.bin", FILE_O_READ);
    if (vf) {
        uint32_t magic = 0;
        vf.read((uint8_t *)&magic, 4);
        vf.close();
        if (magic == 0x47454F31) {
            Serial.println("  Geo database: OK (magic verified)");
        } else {
            Serial.println("  Geo database: BAD MAGIC");
        }
    } else {
        Serial.println("  Geo database: MISSING");
    }

    // Report storage usage
    uint32_t used = bbsExtFS.usedBytes();
    uint32_t total = bbsExtFS.totalBytes();
    Serial.println();
    Serial.print("Storage: ");
    Serial.print(used / 1024);
    Serial.print("KB / ");
    Serial.print(total / 1024);
    Serial.println("KB");

    Serial.println();
    Serial.println("========================================");
    Serial.println("  DONE! Flash the BBS firmware now.");
    Serial.println("  Double-press reset → copy BBS UF2");
    Serial.println("========================================");

    digitalWrite(LED_GREEN, LOW);
    blinkOK(); // rapid blink = success
}

void loop() {
    // never reached
}
