#ifdef NRF52_SERIES

#include "BBSExtFlash.h"
#include "DebugConfiguration.h"
#include <Arduino.h>
#include <nrfx_qspi.h>
#include <cstring>

// MX25R1635F / WP25R1635F specs
#define EXTFLASH_PAGE_SIZE    256
#define EXTFLASH_BLOCK_COUNT  (EXTFLASH_TOTAL_SIZE / EXTFLASH_SECTOR_SIZE) // 512

// ── QSPI scratch buffer (word-aligned, in RAM) ────────────────────────────

static uint8_t _qspi_scratch[EXTFLASH_PAGE_SIZE] __attribute__((aligned(4)));
static bool _qspi_initialised = false;

// ── QSPI hardware init ────────────────────────────────────────────────────

static bool _qspi_probe_failed = false;  // sticky — don't retry if flash is absent
static char _qspi_diag[48] = "not probed";  // diagnostic string for stats

const char *extFlashDiag() { return _qspi_diag; }

static bool _qspi_hw_init(void) {
    if (_qspi_initialised) return true;
    if (_qspi_probe_failed) return false;  // already tried, flash not present

#ifndef PIN_QSPI_SCK
    // No QSPI pins defined for this board
    snprintf(_qspi_diag, sizeof(_qspi_diag), "no QSPI pins");
    return false;
#else

#ifndef NRFX_QSPI_DEFAULT_CONFIG_IRQ_PRIORITY
#define NRFX_QSPI_DEFAULT_CONFIG_IRQ_PRIORITY 6
#endif

    LOG_INFO("[ExtFlash] Probing QSPI flash...\n");

    nrfx_qspi_config_t cfg = NRFX_QSPI_DEFAULT_CONFIG(
        PIN_QSPI_SCK, PIN_QSPI_CS,
        PIN_QSPI_IO0, PIN_QSPI_IO1, PIN_QSPI_IO2, PIN_QSPI_IO3);

    // Conservative clock: 8 MHz (64/8) in single-line mode
    cfg.phy_if.sck_freq = NRF_QSPI_FREQ_DIV8;

    nrfx_err_t err = nrfx_qspi_init(&cfg, NULL, NULL); // blocking mode
    if (err == NRFX_ERROR_INVALID_STATE) {
        // Already initialized (e.g., by bootloader handoff) — that's fine
        _qspi_initialised = true;
        LOG_INFO("[ExtFlash] QSPI already initialized\n");
    } else if (err != NRFX_SUCCESS) {
        LOG_ERROR("[ExtFlash] nrfx_qspi_init failed: %d\n", (int)err);
        snprintf(_qspi_diag, sizeof(_qspi_diag), "init err %d", (int)err);
        _qspi_probe_failed = true;
        return false;
    } else {
        _qspi_initialised = true;
        LOG_INFO("[ExtFlash] QSPI peripheral initialized at 8 MHz\n");
    }

    // Probe: Read JEDEC ID (command 0x9F) — 3 bytes: manufacturer, type, capacity
    {
        uint8_t jedec[3] = {0, 0, 0};
        nrf_qspi_cinstr_conf_t rdid = {
            .opcode    = 0x9F, // Read JEDEC ID
            .length    = NRF_QSPI_CINSTR_LEN_4B, // opcode + 3 data bytes
            .io2_level = true,
            .io3_level = true,
            .wipwait   = false,
            .wren      = false,
        };
        nrfx_err_t jerr = nrfx_qspi_cinstr_xfer(&rdid, NULL, jedec);
        if (jerr != NRFX_SUCCESS) {
            LOG_ERROR("[ExtFlash] JEDEC ID read failed: %d\n", (int)jerr);
            snprintf(_qspi_diag, sizeof(_qspi_diag), "JEDEC err %d", (int)jerr);
            nrfx_qspi_uninit();
            _qspi_initialised = false;
            _qspi_probe_failed = true;
            return false;
        }

        LOG_INFO("[ExtFlash] JEDEC ID: %02X %02X %02X\n", jedec[0], jedec[1], jedec[2]);
        snprintf(_qspi_diag, sizeof(_qspi_diag), "JEDEC %02X:%02X:%02X", jedec[0], jedec[1], jedec[2]);

        // Check for valid manufacturer IDs:
        //   0xC2 = Macronix (MX25R1635F)
        //   0x9D = ISSI (IS25LP080D)
        //   0xEF = Winbond (W25Q16JV)
        //   0x85 = Puya (P25Q16H)
        //   0xBA = Zetta (ZD25WQ32C)
        if (jedec[0] == 0x00 || jedec[0] == 0xFF) {
            LOG_ERROR("[ExtFlash] No flash chip detected (JEDEC=%02X %02X %02X)\n",
                      jedec[0], jedec[1], jedec[2]);
            nrfx_qspi_uninit();
            _qspi_initialised = false;
            _qspi_probe_failed = true;
            return false;
        }
    }

    // Clear block protection bits — IS25LP080D (RAK4631) may ship with all blocks protected.
    {
        nrf_qspi_cinstr_conf_t wren = {
            .opcode    = 0x06, // Write Enable
            .length    = NRF_QSPI_CINSTR_LEN_1B,
            .io2_level = true,
            .io3_level = true,
            .wipwait   = false,
            .wren      = false,
        };
        nrfx_qspi_cinstr_xfer(&wren, NULL, NULL);

        uint8_t sr_val = 0x00; // Clear all protection bits
        nrf_qspi_cinstr_conf_t wrsr = {
            .opcode    = 0x01, // Write Status Register
            .length    = NRF_QSPI_CINSTR_LEN_2B, // opcode + 1 data byte
            .io2_level = true,
            .io3_level = true,
            .wipwait   = true,
            .wren      = false,
        };
        nrfx_qspi_cinstr_xfer(&wrsr, &sr_val, NULL);
        LOG_INFO("[ExtFlash] Cleared block protection bits\n");
    }

    return true;
#endif // PIN_QSPI_SCK
}

// ── LittleFS callbacks ─────────────────────────────────────────────────────

static int _extflash_read(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, void *buffer, lfs_size_t size) {
    (void)c;
    uint32_t addr = block * EXTFLASH_SECTOR_SIZE + off;

    // nrfx_qspi_read requires word-aligned RAM buffer and word-aligned size
    if (((uintptr_t)buffer & 3) == 0 && (size & 3) == 0) {
        nrfx_err_t err = nrfx_qspi_read(buffer, size, addr);
        if (err != NRFX_SUCCESS) {
            LOG_ERROR("[ExtFlash] read fail @0x%lx sz=%lu err=%d\n",
                      (unsigned long)addr, (unsigned long)size, (int)err);
            return LFS_ERR_IO;
        }
    } else {
        // Unaligned: read through scratch buffer in chunks
        uint8_t *dst = (uint8_t *)buffer;
        while (size > 0) {
            uint32_t chunk = (size > sizeof(_qspi_scratch)) ? sizeof(_qspi_scratch) : size;
            uint32_t qchunk = (chunk + 3) & ~3u;
            nrfx_err_t err = nrfx_qspi_read(_qspi_scratch, qchunk, addr);
            if (err != NRFX_SUCCESS) {
                LOG_ERROR("[ExtFlash] scratch read fail @0x%lx\n", (unsigned long)addr);
                return LFS_ERR_IO;
            }
            memcpy(dst, _qspi_scratch, chunk);
            dst  += chunk;
            addr += chunk;
            size -= chunk;
        }
    }
    return LFS_ERR_OK;
}

static int _extflash_prog(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)c;
    uint32_t addr = block * EXTFLASH_SECTOR_SIZE + off;

    if (((uintptr_t)buffer & 3) == 0 && (size & 3) == 0) {
        nrfx_err_t err = nrfx_qspi_write(buffer, size, addr);
        if (err != NRFX_SUCCESS) {
            LOG_ERROR("[ExtFlash] write fail @0x%lx sz=%lu err=%d\n",
                      (unsigned long)addr, (unsigned long)size, (int)err);
            return LFS_ERR_IO;
        }
    } else {
        const uint8_t *src = (const uint8_t *)buffer;
        while (size > 0) {
            uint32_t chunk = (size > sizeof(_qspi_scratch)) ? sizeof(_qspi_scratch) : size;
            memcpy(_qspi_scratch, src, chunk);
            uint32_t qchunk = (chunk + 3) & ~3u;
            for (uint32_t i = chunk; i < qchunk; i++) _qspi_scratch[i] = 0xFF;
            nrfx_err_t err = nrfx_qspi_write(_qspi_scratch, qchunk, addr);
            if (err != NRFX_SUCCESS) {
                LOG_ERROR("[ExtFlash] scratch write fail @0x%lx\n", (unsigned long)addr);
                return LFS_ERR_IO;
            }
            src  += chunk;
            addr += chunk;
            size -= chunk;
        }
    }

    // Wait for write to complete before returning
    while (nrfx_qspi_mem_busy_check() == NRFX_ERROR_BUSY) {
        yield();
    }
    return LFS_ERR_OK;
}

static int _extflash_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    uint32_t addr = block * EXTFLASH_SECTOR_SIZE;
    nrfx_err_t err = nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, addr);
    if (err != NRFX_SUCCESS) {
        LOG_ERROR("[ExtFlash] erase fail @0x%lx err=%d\n", (unsigned long)addr, (int)err);
        return LFS_ERR_IO;
    }
    // Wait for erase to complete
    while (nrfx_qspi_mem_busy_check() == NRFX_ERROR_BUSY) {
        yield();
    }
    return LFS_ERR_OK;
}

static int _extflash_sync(const struct lfs_config *c) {
    (void)c;
    while (nrfx_qspi_mem_busy_check() == NRFX_ERROR_BUSY) {
        yield();
    }
    return LFS_ERR_OK;
}

// ── LittleFS config & buffers (single copy, in this TU only) ──────────────

static uint8_t _extlfs_read_buf[EXTFLASH_PAGE_SIZE] __attribute__((aligned(4)));
static uint8_t _extlfs_prog_buf[EXTFLASH_PAGE_SIZE] __attribute__((aligned(4)));
static uint8_t _extlfs_lookahead_buf[64] __attribute__((aligned(4)));

static struct lfs_config _extFlashConfig = {
    .context = NULL,
    .read    = _extflash_read,
    .prog    = _extflash_prog,
    .erase   = _extflash_erase,
    .sync    = _extflash_sync,

    .read_size      = EXTFLASH_PAGE_SIZE,
    .prog_size      = EXTFLASH_PAGE_SIZE,
    .block_size     = EXTFLASH_SECTOR_SIZE,
    .block_count    = EXTFLASH_BLOCK_COUNT,
    .lookahead      = 512,

    .read_buffer      = _extlfs_read_buf,
    .prog_buffer      = _extlfs_prog_buf,
    .lookahead_buffer = _extlfs_lookahead_buf,
    .file_buffer      = NULL,
};

// ── ExternalFileSystem implementation ──────────────────────────────────────

ExternalFileSystem::ExternalFileSystem()
    : Adafruit_LittleFS(&_extFlashConfig) {}

bool ExternalFileSystem::begin() {
    if (_mounted) return true;
    if (!_qspi_hw_init()) {
        LOG_ERROR("[ExtFlash] QSPI init failed\n");
        return false;
    }

    // Try to mount existing filesystem
    if (Adafruit_LittleFS::begin()) {
        LOG_INFO("[ExtFlash] Mounted 2MB external flash\n");
        _mounted = true;
        return true;
    }

    // First boot or corrupted: format then mount
    LOG_WARN("[ExtFlash] Mount failed, formatting...\n");
    if (!this->format()) {
        LOG_ERROR("[ExtFlash] Format failed\n");
        return false;
    }
    if (!Adafruit_LittleFS::begin()) {
        LOG_ERROR("[ExtFlash] Mount after format failed\n");
        return false;
    }
    LOG_INFO("[ExtFlash] Formatted and mounted 2MB external flash\n");
    _mounted = true;
    return true;
}

bool ExternalFileSystem::isAvailable() {
    return _mounted;
}

uint32_t ExternalFileSystem::usedBytes() {
    uint32_t count = 0;
    lfs_traverse(_getFS(), [](void *ctx, lfs_block_t) -> int {
        (*(uint32_t *)ctx)++;
        return 0;
    }, &count);
    return count * EXTFLASH_SECTOR_SIZE;
}

// Lazy singleton — avoids global constructor running before FreeRTOS scheduler
ExternalFileSystem &bbsExtFS() {
    static ExternalFileSystem instance;
    return instance;
}

#endif // NRF52_SERIES
