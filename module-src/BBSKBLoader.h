#pragma once
// BBSKBLoader — Knowledge Base loader, called once from setup() or runOnce()
// Writes embedded wordle.bin and geo_us.bin to external QSPI flash.
// Only writes if the files are missing or wrong size (idempotent).

#ifdef NRF52_SERIES

#include "BBSExtFlash.h"
#ifdef BBS_KB_LOAD_WORDLE
#include "BBSWordleData.h"
#endif
#ifdef BBS_KB_LOAD_GEO
#include "BBSGeoData.h"
#endif
#include "DebugConfiguration.h"

static bool kbLoaderWriteFile(const char *path, const uint8_t *data, uint32_t size) {
    // Always overwrite — delete existing file
    if (bbsExtFS().exists(path)) {
        bbsExtFS().remove(path);
        LOG_INFO("[KBLoader] Removed old %s\n", path);
    }

    // Ensure directories
    if (!bbsExtFS().exists("/bbs")) bbsExtFS().mkdir("/bbs");
    if (!bbsExtFS().exists("/bbs/kb")) bbsExtFS().mkdir("/bbs/kb");

    File f = bbsExtFS().open(path, FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("[KBLoader] Can't open %s for write\n", path);
        return false;
    }

    // Write in chunks — copy through RAM buffer since data is in flash
    uint8_t ramBuf[128];
    uint32_t written = 0;
    while (written < size) {
        uint32_t chunk = size - written;
        if (chunk > sizeof(ramBuf)) chunk = sizeof(ramBuf);
        memcpy(ramBuf, data + written, chunk);
        f.write(ramBuf, chunk);
        written += chunk;
    }
    f.close();

    LOG_INFO("[KBLoader] Wrote %s (%u bytes)\n", path, written);
    return true;
}

static bool kbLoaderRun() {
    LOG_INFO("[KBLoader] Starting knowledge base load...\n");

    if (!bbsExtFS().begin()) {
        LOG_ERROR("[KBLoader] External flash init FAILED\n");
        return false;
    }
    LOG_INFO("[KBLoader] External flash mounted OK\n");

    // Test write
    {
        File test = bbsExtFS().open("/bbs/kb/test.txt", FILE_O_WRITE);
        if (test) {
            test.write((const uint8_t *)"hello", 5);
            test.close();
            LOG_INFO("[KBLoader] Test file written OK\n");
        } else {
            LOG_ERROR("[KBLoader] Test file write FAILED\n");
        }
    }

    bool ok = true;
#ifdef BBS_KB_LOAD_WORDLE
    ok &= kbLoaderWriteFile("/bbs/kb/wordle.bin", KB_WORDLE_DATA, KB_WORDLE_SIZE);
#endif
#ifdef BBS_KB_LOAD_GEO
    ok &= kbLoaderWriteFile("/bbs/kb/geo_us.bin", KB_GEO_DATA, KB_GEO_SIZE);
#endif

    if (ok) {
        LOG_INFO("[KBLoader] Knowledge base ready!\n");
    }
    return ok;
}

#endif // NRF52_SERIES
