#pragma once
// BBSGeoLookup — Offline reverse geocoding
//
// Two modes:
//   1. External flash: reads /bbs/kb/geo_us.bin (packed binary with spatial index, 378KB)
//   2. Embedded fallback: 500-city const array (BBSGeoDB.h, 11KB in firmware)
//
// Tries external flash first. Falls back to embedded if file missing.

#ifndef NRF52_SERIES
#include "BBSGeoDB.h"
#endif
#include <cmath>
#include <cstring>

// Constants
#define GEO_DB_PATH     "/bbs/kb/geo_us.bin"
#define GEO_MAGIC       0x47454F31
#define GEO_MAX_DIST_DEG2  (0.7f * 0.7f)  // ~50 miles

// ── Embedded fallback (500 cities compiled in, ESP32 only) ─────────────────
#ifndef NRF52_SERIES
static bool geoLookupNearest(float lat, float lon, char *city, size_t cityLen) {
    if (!city || cityLen == 0) return false;
    city[0] = '\0';

    float bestDist = GEO_MAX_DIST_DEG2;
    int   bestIdx  = -1;

    for (int i = 0; i < GEO_CITY_COUNT; i++) {
        float clat = GEO_CITIES[i].lat100 / 100.0f;
        float clon = GEO_CITIES[i].lon100 / 100.0f;
        float dlat = lat - clat;
        float dlon = (lon - clon) * cosf(lat * 0.01745329f);
        float d2   = dlat * dlat + dlon * dlon;
        if (d2 < bestDist) {
            bestDist = d2;
            bestIdx  = i;
        }
    }

    if (bestIdx < 0) return false;
    strncpy(city, GEO_CITIES[bestIdx].name, cityLen - 1);
    city[cityLen - 1] = '\0';
    return true;
}
#endif // !NRF52_SERIES

// ── External flash lookup (packed binary with spatial index) ───────────────

#ifdef NRF52_SERIES
#include "BBSExtFlash.h"

static bool geoLookupFromExtFlash(float lat, float lon, char *city, size_t cityLen) {
    if (!city || cityLen == 0) return false;
    city[0] = '\0';

    File f = bbsExtFS().open(GEO_DB_PATH, FILE_O_READ);
    if (!f) return false;

    // Read header
    uint32_t magic = 0, cellCount = 0;
    if (f.read((uint8_t *)&magic, 4) != 4 || magic != GEO_MAGIC) { f.close(); return false; }
    if (f.read((uint8_t *)&cellCount, 4) != 4 || cellCount == 0) { f.close(); return false; }

    // Compute cell key
    int16_t  targetLat = (int16_t)floorf(lat);
    uint16_t targetLon = (uint16_t)((int)floorf(lon) + 180);

    // Binary search the index (12 bytes per entry: lat(i16) + lon_off(u16) + offset(u32) + count(u16) + pad(u16))
    uint32_t lo = 0, hi = cellCount;
    uint32_t dataOffset = 0;
    uint16_t entryCount = 0;
    bool found = false;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t seekPos = 8 + mid * 12;  // header=8, index entry=12
        f.seek(seekPos);

        int16_t  eLat;
        uint16_t eLon;
        uint32_t eOff;
        uint16_t eCnt;
        uint16_t ePad;
        f.read((uint8_t *)&eLat, 2);
        f.read((uint8_t *)&eLon, 2);
        f.read((uint8_t *)&eOff, 4);
        f.read((uint8_t *)&eCnt, 2);
        f.read((uint8_t *)&ePad, 2);

        if (eLat < targetLat || (eLat == targetLat && eLon < targetLon)) {
            lo = mid + 1;
        } else if (eLat > targetLat || (eLat == targetLat && eLon > targetLon)) {
            hi = mid;
        } else {
            dataOffset = eOff;
            entryCount = eCnt;
            found = true;
            break;
        }
    }

    if (!found || entryCount == 0) { f.close(); return false; }

    // Read cell entries (22 bytes each: lat100(i16) + lon100(i16) + name(18))
    f.seek(dataOffset);

    float bestDist = GEO_MAX_DIST_DEG2;
    char  bestName[18] = {0};

    for (uint16_t i = 0; i < entryCount; i++) {
        int16_t clat100, clon100;
        char name[18];
        f.read((uint8_t *)&clat100, 2);
        f.read((uint8_t *)&clon100, 2);
        f.read((uint8_t *)name, 18);

        float clat = clat100 / 100.0f;
        float clon = clon100 / 100.0f;
        float dlat = lat - clat;
        float dlon = (lon - clon) * cosf(lat * 0.01745329f);
        float d2 = dlat * dlat + dlon * dlon;
        if (d2 < bestDist) {
            bestDist = d2;
            memcpy(bestName, name, 18);
            bestName[17] = '\0';
        }
    }

    f.close();

    if (bestName[0] == '\0') return false;
    strncpy(city, bestName, cityLen - 1);
    city[cityLen - 1] = '\0';
    return true;
}
#endif // NRF52_SERIES

// ── Combined lookup: try external flash first, fall back to embedded ───────

static bool geoLookup(float lat, float lon, char *city, size_t cityLen) {
#ifdef NRF52_SERIES
    return geoLookupFromExtFlash(lat, lon, city, cityLen);
#else
    return geoLookupNearest(lat, lon, city, cityLen);
#endif
}
