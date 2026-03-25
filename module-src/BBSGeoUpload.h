#pragma once
// BBSGeoUpload — Copy geo database from internal LittleFS to external QSPI flash
//
// Workflow:
//   1. Run: python3 scripts/gen_geo_packed.py     → creates data/geo_us.bin
//   2. Run: pio run -e t-echo -t uploadfs          → writes data/ to internal LittleFS
//   3. On next boot, this code copies /geo_us.bin from FSCom to bbsExtFS
//   4. Once copied, the internal copy is deleted to free space
//
// The platformio.ini data_dir points to ../../data which contains geo_us.bin

#ifdef NRF52_SERIES

#include "FSCommon.h"
#include "BBSExtFlash.h"
#include "BBSGeoLookup.h"
#include "DebugConfiguration.h"

static bool geoMigrateToExtFlash() {
    // Skip if already on external flash
    if (bbsExtFS().exists(GEO_DB_PATH)) {
        LOG_INFO("[Geo] Database already on external flash\n");
        return true;
    }

    // Check if source exists on internal flash
    const char *srcPath = "/geo_us.bin";
    if (!FSCom.exists(srcPath)) {
        LOG_INFO("[Geo] No geo database on internal flash (run: pio run -t uploadfs)\n");
        return false;
    }

    LOG_INFO("[Geo] Migrating geo database to external flash...\n");

    File src = FSCom.open(srcPath, FILE_O_READ);
    if (!src) {
        LOG_ERROR("[Geo] Failed to open source\n");
        return false;
    }

    // Ensure /bbs directory exists on external flash
    if (!bbsExtFS().exists("/bbs")) bbsExtFS().mkdir("/bbs");

    File dst = bbsExtFS().open(GEO_DB_PATH, FILE_O_WRITE);
    if (!dst) {
        LOG_ERROR("[Geo] Failed to open destination\n");
        src.close();
        return false;
    }

    uint8_t buf[256];
    uint32_t total = 0;
    while (src.available()) {
        int n = src.read(buf, sizeof(buf));
        if (n <= 0) break;
        dst.write(buf, n);
        total += n;
    }

    dst.close();
    src.close();

    LOG_INFO("[Geo] Copied %lu bytes to external flash\n", (unsigned long)total);

    // Verify by checking magic
    File verify = bbsExtFS().open(GEO_DB_PATH, FILE_O_READ);
    if (verify) {
        uint32_t magic = 0;
        verify.read((uint8_t *)&magic, 4);
        verify.close();
        if (magic == GEO_MAGIC) {
            // Success — delete internal copy to free space
            FSCom.remove(srcPath);
            LOG_INFO("[Geo] Migration complete, internal copy removed\n");
            return true;
        }
    }

    LOG_ERROR("[Geo] Migration verification failed\n");
    return false;
}

#endif // NRF52_SERIES
