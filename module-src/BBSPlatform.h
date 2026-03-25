#pragma once
// Cross-platform compatibility for ESP32 (Arduino LittleFS) vs nRF52 (Adafruit LittleFS)
//
// Key differences:
//   ESP32:  File f;                 // default constructor OK
//           FSCom.open(path, "r")   // string mode
//           FSCom.totalBytes() / usedBytes()
//
//   nRF52:  File f(FSCom);          // requires filesystem reference
//           FSCom.open(path, FILE_O_READ)  // integer mode (same macro works)
//           no totalBytes()/usedBytes() on InternalFileSystem

#ifdef NRF52_SERIES
  // nRF52: File must be constructed with a filesystem reference
  #define BBS_FILE_VAR(name)        File name(FSCom)
  #define BBS_FILE_APPEND_MODE      FILE_O_WRITE   // no true append; caller must handle
  #define BBS_FS_FREE_BYTES()       (0UL)  // internal FS has no size query
  // External flash macros (for BBSStorageExtFlash)
  #define BBS_EXTFS_FILE_VAR(name)  File name(bbsExtFS())
#else
  // ESP32 / other Arduino platforms
  #define BBS_FILE_VAR(name)        File name
  #define BBS_FILE_APPEND_MODE      "a"
  #define BBS_FS_FREE_BYTES()       ((size_t)(FSCom.totalBytes() - FSCom.usedBytes()))
#endif
