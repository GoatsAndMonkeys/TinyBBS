#pragma once
// External QSPI Flash LittleFS for nRF52840 (T-Echo)
//
// Provides a standalone Adafruit_LittleFS filesystem on the external
// MX25R1635F / WP25R1635F 2MB QSPI flash chip, completely separate from
// Meshtastic's InternalFS.  This gives the BBS ~2MB of dedicated storage
// instead of fighting for the ~28KB internal LittleFS partition.
//
// Usage:
//   #include "BBSExtFlash.h"
//   if (bbsExtFS.begin()) { /* mounted */ }
//   File f = bbsExtFS.open("/bbs/foo.bin", FILE_O_WRITE);

#ifdef NRF52_SERIES

#include "Adafruit_LittleFS.h"

#define EXTFLASH_TOTAL_SIZE   (2 * 1024 * 1024)   // 2 MB
#define EXTFLASH_SECTOR_SIZE  4096                  // 4 KB erase sector

class ExternalFileSystem : public Adafruit_LittleFS {
  public:
    ExternalFileSystem();
    bool begin();
    uint32_t totalBytes() const { return EXTFLASH_TOTAL_SIZE; }
    uint32_t usedBytes();
};

// Lazy-initialized pointer — avoids global constructor mutex issue with FreeRTOS
ExternalFileSystem &bbsExtFS();

#endif // NRF52_SERIES
